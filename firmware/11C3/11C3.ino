#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "src/config.h"
#include "src/protocol.h"
#include "src/rmt_ook.h"
#include "src/storage.h"
#include "src/player.h"
#include "src/button.h"
#include "src/ble_service.h"
#include "src/wifi_ap.h"

Storage   storage;
RmtOok    rf;
Player    player;
Button    button;
BleService ble;
WifiAp    wifi;

#ifdef PIN_STATUS_LED
Adafruit_NeoPixel led(1, PIN_STATUS_LED, NEO_GRB + NEO_KHZ800);
bool ledReady = false;
#endif

// ---- 433MHz 转发(来自 PC 的 STREAM 帧) ----
static uint8_t lastFwd[STREAM_PAYLOAD_LEN];
static size_t lastFwdLen = 0;

// ---- 433 协议穷举扫描状态 ----
static bool scanActive = false;
static uint8_t scanBaudIdx = 0, scanModeIdx = 0, scanPreIdx = 0, scanTplIdx = 0;
static uint32_t scanLast = 0;

void forwardTlv(uint8_t cmd, const uint8_t* payload, uint8_t plen) {
  if (cmd != CMD_STREAM) {
    // AUTH 等控制帧只记录, 不转发到空口
#ifdef FIRMWARE_LOG
    Serial.printf("[auth] cmd=0x%02X len=%u\n", cmd, plen);
#endif
    return;
  }
  if (plen != STREAM_PAYLOAD_LEN) return;
  // 独立播放进行中: PC 帧让位
  if (player.state() == Player::PLAYING) return;

  // 去重: 相同帧跳过 (UDP 重复发送/重连重发)
  if (plen == lastFwdLen && memcmp(payload, lastFwd, plen) == 0) return;
  memcpy(lastFwd, payload, plen);
  lastFwdLen = plen;
  // 空口发送: 转为场控 D8 帧
  rf.sendFieldD8(payload);
}

// ---- 串口文本命令(上位机上传/控制) ----
static String serialLine;
static void handleSerialCommand(const String& cmd) {
  // 穷举扫描命令在这里拦截
  if (cmd.startsWith("SCAN")) {
    scanActive = true;
    scanBaudIdx = scanModeIdx = scanPreIdx = scanTplIdx = 0;
    scanLast = millis();
    scanApplyCombination();
    scanEmit();
    return;
  }
  if (cmd.startsWith("STOPSCAN")) {
    scanActive = false;
    return;
  }
  String reply = ble.handleCommand(cmd);
#ifdef FIRMWARE_LOG
  Serial.println("[cmd] " + cmd + " -> " + reply);
#endif
}

void handleSerialByte(uint8_t b) {
  if (b == '\n') {
    String line = serialLine;
    serialLine = "";
    line.trim();
    if (line.length() == 0) return;
    if (line.startsWith("UP:")) {
      String r = ble.handleUpload(line);   // 通过 BLE 上传状态机 beginCsvWrite
#ifdef FIRMWARE_LOG
      Serial.println("[up] " + r);
#endif
      return;
    }
    if (line.startsWith("END:")) {
      String r = ble.handleUpload(line);
#ifdef FIRMWARE_LOG
      Serial.println("[up] " + r);
#endif
      return;
    }
    // 上传中的数据行: 直接同步写入 (串口上下文安全, 不走 BLE 内存缓冲)
    if (ble.uploading()) {
      ble.handleUploadSync(line);
      return;
    }
    if (line.startsWith("PLAY") || line.startsWith("STOP") || line.startsWith("PAUSE") ||
        line.startsWith("NEXT") || line.startsWith("BRIGHT:") || line.startsWith("LOAD:") ||
        line.startsWith("DELETE:") || line.startsWith("LIST") || line.startsWith("STATUS") ||
        line.startsWith("COLOR:") || line.startsWith("RESET")) {
      handleSerialCommand(line);
      return;
    }
    // 调参命令 (RF/WiFi/AP/BLE 共用同一套)
    if (line.startsWith("CFG") || line.startsWith("GETCFG") ||
        line.startsWith("RFBAUD:") || line.startsWith("RFPRE:") ||
        line.startsWith("RFMODE:") || line.startsWith("RFINV:") ||
        line.startsWith("LOOP:") || line.startsWith("SETAP:") ||
        line.startsWith("SETSTA:") || line.startsWith("REBOOT") ||
        line.startsWith("SCAN") || line.startsWith("STOPSCAN")) {
      handleSerialCommand(line);
      return;
    }
    // 其他文本: 若是上传中的数据行则喂给上传缓冲
    String rr = ble.handleCommand(line);
#ifdef FIRMWARE_LOG
    if (!line.startsWith("UP:") && rr.length() > 0) Serial.println("[cmd] " + line + " -> " + rr);
#endif
    return;
  }
  if (b == '\r') return;
  if (serialLine.length() < 512) serialLine += (char)b;
}

// ---- 433 协议穷举扫描 ----
// 自动循环发射不同参数组合, 用于排查未知接收端能识别的编码参数
// 模式0: NRZ/UART 位流 (旧); 模式1: EV1527 PWM 编码 (SYN480R 最常见配套)
static const uint32_t SCAN_BAUDS[]   = {1200, 2400, 4800, 9600, 19200, 38400};
static const uint8_t  SCAN_PRES[]    = {0, 4};
static const uint32_t SCAN_EV_T[]    = {200, 260, 320, 350, 400, 500, 650, 800};
static const uint32_t SCAN_EV_ADDR[] = {
  0x00000, 0xFFFFF, 0xAAAAA, 0x55555, 0x11111, 0xEEEEE, 0x12345, 0x6789A, 0xABCDE, 0x00001
};
static const int SCAN_BAUD_N = sizeof(SCAN_BAUDS) / sizeof(uint32_t);
static const int SCAN_PRE_N  = sizeof(SCAN_PRES);
static const int SCAN_EV_T_N   = sizeof(SCAN_EV_T) / sizeof(uint32_t);
static const int SCAN_EV_ADDR_N = sizeof(SCAN_EV_ADDR) / sizeof(uint32_t);
#define SCAN_TPL_N 7
// EV1527 按键码扫描范围
#define SCAN_EV_KEY_MIN 0
#define SCAN_EV_KEY_MAX 15

static uint8_t scanProto = 0;   // 0=位流, 1=EV1527
static uint8_t scanEvTIdx = 0, scanEvAddrIdx = 0, scanEvKeyIdx = 0;

// 帧内容模板 (穷举常见命令帧结构)
static const uint8_t* scanTemplate(int tpl) {
  static uint8_t t0[10] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  static uint8_t t1[10] = {0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA};
  static uint8_t t2[10] = {0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55};
  static uint8_t t3[10] = {0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00};
  static uint8_t t4[10] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
  static uint8_t t5[10] = {0xF0,0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,0x0F};
  static uint8_t t6[8]  = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
  switch (tpl) {
    case 0: return t0;
    case 1: return t1;
    case 2: return t2;
    case 3: return t3;
    case 4: return t4;
    case 5: return t5;
    default: return t6;
  }
}
static uint8_t scanTemplateLen(int tpl) { return (tpl == 6) ? 8 : 10; }

static void scanApplyCombination() {
  uint32_t baud = SCAN_BAUDS[scanBaudIdx];
  RmtOok::Mode mode = (scanModeIdx == 0) ? RmtOok::MODE_NRZ : RmtOok::MODE_UART;
  uint8_t pre = SCAN_PRES[scanPreIdx];
  rf.setBaud(baud);
  rf.setMode(mode);
  rf.setPreamble(pre);
}

static void scanEmit() {
  if (scanProto == 1) {
    // EV1527: 地址(20bit)<<4 | 按键码(4bit)
    uint32_t code = (SCAN_EV_ADDR[scanEvAddrIdx] << 4) | (uint32_t)(scanEvKeyIdx & 0x0F);
    rf.sendEv1527(code, 8, SCAN_EV_T[scanEvTIdx]);
#ifdef FIRMWARE_LOG
    Serial.printf("[scan] EV1527 T=%u addr=0x%05X key=%u code=0x%06X\r\n",
                  SCAN_EV_T[scanEvTIdx], SCAN_EV_ADDR[scanEvAddrIdx], scanEvKeyIdx, code);
#endif
    return;
  }
  const uint8_t* t = scanTemplate(scanTplIdx);
  int len = scanTemplateLen(scanTplIdx);
  // 连续发 6 次, 覆盖接收窗口
  for (int i = 0; i < 6; i++) rf.send(t, len);
#ifdef FIRMWARE_LOG
  Serial.printf("[scan] baud=%u mode=%d pre=%u tpl=%d\r\n",
                SCAN_BAUDS[scanBaudIdx], scanModeIdx, SCAN_PRES[scanPreIdx], scanTplIdx);
#endif
}

static void scanTick() {
  if (!scanActive) return;
  if (millis() - scanLast < 3000) return;
  scanLast = millis();
  // 切到下一组合
  if (scanProto == 1) {
    scanEvKeyIdx++;
    if (scanEvKeyIdx > SCAN_EV_KEY_MAX) {
      scanEvKeyIdx = 0;
      scanEvAddrIdx++;
      if (scanEvAddrIdx >= SCAN_EV_ADDR_N) {
        scanEvAddrIdx = 0;
        scanEvTIdx++;
        if (scanEvTIdx >= SCAN_EV_T_N) {
          scanEvTIdx = 0;
          scanProto = 0;   // EV1527 扫完回到位流
        }
      }
    }
  } else {
    scanTplIdx++;
    if (scanTplIdx >= SCAN_TPL_N) {
      scanTplIdx = 0;
      scanModeIdx++;
      if (scanModeIdx >= 2) {
        scanModeIdx = 0;
        scanPreIdx++;
        if (scanPreIdx >= SCAN_PRE_N) {
          scanPreIdx = 0;
          scanBaudIdx++;
          if (scanBaudIdx >= SCAN_BAUD_N) {
            scanBaudIdx = 0;
            scanProto = 1;   // 位流扫完切到 EV1527
            scanEvTIdx = scanEvAddrIdx = scanEvKeyIdx = 0;
          }
        }
      }
    }
  }
  scanEmit();
}

// ---- 按钮事件 ----
static uint8_t brightnessCycle[] = {100, 75, 50, 25};
static uint8_t brightnessIdx = 0;

void onButton(BtnEvent ev) {
  switch (ev) {
    case BTN_SHORT: {   // 播放/暂停
      if (player.state() == Player::IDLE) {
        String cur = storage.getCurrentPreset();
        if (cur.length() == 0) { player.toggle(); }  // 会尝试 current
        else player.play(cur);
      } else player.toggle();
      ble.notify(("BTN:play " + player.current()).c_str());
      break;
    }
    case BTN_DOUBLE: {  // 下一节目
      String names[MAX_PRESETS];
      int n = storage.listPresets(names, MAX_PRESETS);
      if (n > 0) {
        player.next(names, n);
        ble.notify(("BTN:next " + player.current()).c_str());
      }
      break;
    }
    case BTN_LONG: {    // 亮度循环
      brightnessIdx = (brightnessIdx + 1) % 4;
      uint8_t pct = brightnessCycle[brightnessIdx];
      storage.setBrightness(pct);
      player.setBrightness(pct);
      player.resendCurrent();
      ble.notify(("BTN:bright " + String(pct)).c_str());
      break;
    }
    case BTN_VERY_LONG: {  // 恢复出厂
      ble.notify("BTN:factory reset");
      player.stop();
      storage.eraseAll();
      delay(100);
      ESP.restart();
      break;
    }
    default: break;
  }
}

// ---- 状态灯 ----
void updateLed() {
#ifdef PIN_STATUS_LED
  if (!ledReady) return;
  uint32_t color = 0x0000FF;   // 空闲=蓝
  if (player.state() == Player::PLAYING) color = 0x00FF00;   // 播放=绿
  else if (player.state() == Player::PAUSED) color = 0xFFFF00; // 暂停=黄
  if (wifi.staConnected()) color = 0x00FFFF;                 // 已连路由器=青
  led.setPixelColor(0, color);
  led.show();
#endif
}

// ---- 串口 TLV 解析 ----
TlvParser serialParser;

void setup() {
  Serial.setRxBufferSize(4096);   // 上传大 CSV 行/批量数据需要大 RX 缓冲
  Serial.begin(115200);
  delay(100);
#ifdef FIRMWARE_LOG
  Serial.println("\n=== 11C3 boot ===");
#endif

  storage.begin();

  // RF 参数
  rf.begin((gpio_num_t)PIN_ASK_TX);
  rf.setBaud(storage.getRfBaud());
  rf.setMode((RmtOok::Mode)storage.getRfMode());
  rf.setInvert(storage.getRfInvert());
  rf.setPreamble(storage.getRfPreamble());

  player.begin(&storage, &rf);

  // 串口/上传等解析器
  serialParser.setCallback(forwardTlv);
  ble.begin(&storage, &player, &rf, forwardTlv);
  wifi.begin(&storage, &player, forwardTlv);
  wifi.setNotifyFn([](const char* m){ ble.notify(m); });
  // 网页 /api/cmd 委托给 BLE/串口同一套命令实现
  wifi.setCmdFn([](const String& c){ return ble.handleCommand(c); });

  button.begin(PIN_BUTTON, onButton);

#ifdef PIN_STATUS_LED
  led.begin();
  led.setBrightness(20);
  ledReady = true;
#endif

#ifdef FIRMWARE_LOG
  Serial.printf("[cfg] baud=%u mode=%d inv=%d pre=%u bright=%u\n",
                storage.getRfBaud(), storage.getRfMode(),
                storage.getRfInvert() ? 1 : 0, storage.getRfPreamble(),
                storage.getBrightness());
  Serial.printf("[wifi] AP=%s IP=%s\n", WIFI_AP_SSID_DEFAULT, wifi.apIp().c_str());
#endif

  // 清理旧版内置 demo 预设 (已废弃, 不再内置生成)
  if (storage.presetExists("demo")) {
    storage.deletePreset("demo");
#ifdef FIRMWARE_LOG
    Serial.println("[clean] removed legacy demo preset");
#endif
  }

  // 上电自动播放: 仅播放用户最后使用的节目 (无则保持空闲)
  String cur = storage.getCurrentPreset();
  if (cur.length() > 0 && storage.presetExists(cur)) {
    player.play(cur);
#ifdef FIRMWARE_LOG
    Serial.printf("[auto] play %s\n", cur.c_str());
#endif
  } else {
#ifdef FIRMWARE_LOG
    Serial.printf("[auto] no preset to autoplay\n");
#endif
  }
}

void loop() {
  button.update();

  // 串口: 二进制 TLV 直接喂解析器; 文本行走命令
  while (Serial.available()) {
    uint8_t b = (uint8_t)Serial.read();
    serialParser.feed(b);
    handleSerialByte(b);
  }

  player.tick();
  wifi.update();
  ble.update();
  updateLed();
  scanTick();

  static uint32_t lastLog = 0;
  if (millis() - lastLog > 5000) {
    lastLog = millis();
#ifdef FIRMWARE_LOG
    Serial.printf("[st] %s %s bright=%u sta=%d\n",
                  player.state() == Player::PLAYING ? "playing" :
                  player.state() == Player::PAUSED ? "paused" : "idle",
                  player.current().c_str(), player.brightness(),
                  wifi.staConnected() ? 1 : 0);
#endif
  }
}
