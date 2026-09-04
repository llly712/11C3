#include "ble_service.h"

class LumaWriteCb : public NimBLECharacteristicCallbacks {
public:
  LumaWriteCb(BleService* svc) : _svc(svc) {}
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    std::string v = c->getValue();
    // 逐字节喂 TLV 解析器(电脑端按 MTU 分块写, 一帧可能跨多次写)
    for (size_t i = 0; i < v.size(); i++) {
      _parser.feed((uint8_t)v[i]);
    }
  }
  TlvParser _parser;
private:
  BleService* _svc;
};

class CtrlWriteCb : public NimBLECharacteristicCallbacks {
public:
  CtrlWriteCb(BleService* svc) : _svc(svc) {}
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    std::string v = c->getValue();
    String cmd(v.c_str(), (int)v.size());
    cmd.trim();
    String reply = _svc->handleCommand(cmd);
    if (reply.length() > 0) _svc->notify(reply.c_str());
  }
private:
  BleService* _svc;
};

class UploadWriteCb : public NimBLECharacteristicCallbacks {
public:
  UploadWriteCb(BleService* svc) : _svc(svc) {}
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    std::string v = c->getValue();
    String reply = _svc->handleUpload(String(v.c_str(), (int)v.size()));
    if (reply.length() > 0) _svc->notify(reply.c_str());
  }
private:
  BleService* _svc;
};

class ServerCb : public NimBLEServerCallbacks {
public:
  ServerCb(BleService* svc) : _svc(svc) {}
  void onConnect(NimBLEServer* s, NimBLEConnInfo& ci) override {
    _svc->notify("CONNECTED");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& ci, int reason) override {
    NimBLEDevice::startAdvertising();
  }
private:
  BleService* _svc;
};

void BleService::begin(Storage* storage, Player* player, RmtOok* rf,
                       void (*forwardFn)(uint8_t, const uint8_t*, uint8_t)) {
  _st = storage;
  _player = player;
  _rf = rf;
  _forward = forwardFn;

  NimBLEDevice::init("");
  NimBLEDevice::setMTU(517);

  _server = NimBLEDevice::createServer();
  _server->setCallbacks(new ServerCb(this));

  // ---- 11C3 控制服务 (兼容 LumaFlow 协议) ----
  NimBLEService* luma = _server->createService(BLE_SVC_LUMA);
  NimBLECharacteristic* lumaChr = luma->createCharacteristic(
      BLE_CHR_LUMA, NIMBLE_PROPERTY::WRITE_NR);
  LumaWriteCb* lcb = new LumaWriteCb(this);
  lcb->_parser.setCallback(_forward);
  lumaChr->setCallbacks(lcb);
  luma->start();

  // ---- 控制/上传服务 ----
  NimBLEService* ctrl = _server->createService(BLE_SVC_CTRL);
  NimBLECharacteristic* ctrlChr = ctrl->createCharacteristic(
      BLE_CHR_CTRL, NIMBLE_PROPERTY::WRITE);
  ctrlChr->setCallbacks(new CtrlWriteCb(this));
  NimBLECharacteristic* upChr = ctrl->createCharacteristic(
      BLE_CHR_UPLOAD, NIMBLE_PROPERTY::WRITE);
  upChr->setCallbacks(new UploadWriteCb(this));
  _statusChr = ctrl->createCharacteristic(
      BLE_CHR_STATUS, NIMBLE_PROPERTY::NOTIFY);
  ctrl->start();

  // ---- 广播 ----
  String devName = String(BLE_NAME_PREFIX) + "-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
  NimBLEDevice::setDeviceName(devName.c_str());

  // 用 NimBLEAdvertisementData 显式构造广播数据:
  //  - 主广播: flags + LumaFlow 兼容服务 UUID (7e570001, 128 位)。
  //    原版 LumaFlow 上位机 (github.com/ltyridium/LumaFlow) 靠「名称前缀 LumaFlow
  //    或 广播服务 UUID 7e570001」过滤设备 (core/serial_device_manager.py)。
  //    必须用 setCompleteServices(自动按 UUID 位宽写 128 位)。
  //    注意: 不能再用 setCompleteServices16 —— 它只接受 16 位 UUID,
  //          128 位 UUID 会被 setServices() 静默丢弃, 设备对 LumaFlow 上位机不可见。
  //  - 扫描响应: 设备完整名 (11C3-xxxx), 供 11C3 安卓 App / host_lite 按前缀发现。
  //    31B 主广播放不下 flags + 128位服务UUID + 完整名, 故名字放 scan response。
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);   // LE General Discoverable + BR/EDR Not Supported
  advData.setCompleteServices(NimBLEUUID(BLE_SVC_LUMA));

  NimBLEAdvertisementData scanData;
  scanData.setName(devName.c_str(), true);   // Complete Local Name

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);
  NimBLEDevice::startAdvertising();
}

void BleService::notify(const char* text) {
  if (_statusChr) {
    _statusChr->setValue((uint8_t*)text, strlen(text));
    _statusChr->notify();
  }
}

void BleService::update() {
  // 把缓存的 BLE 上传数据喂给流式写入 (main loop 上下文, 避免在 NimBLE 回调做文件 I/O)
  if (_uploading && _uploadBuf.length() > 0) {
    _st->appendCsvData(_uploadBuf);
    _uploadBuf = "";
  }
  if (_savePending) {
    _savePending = false;
    String err = _st->endCsvWrite();
    if (err.length() == 0) {
      notify(("OK:preset " + _uploadName).c_str());
    } else {
      notify(("ERR:" + err).c_str());
    }
  }
}

String BleService::handleUpload(const String& line) {
  if (line.startsWith("UP:")) {
    _uploadName = _st->sanitizeName(line.substring(3));
    if (_uploading) { _st->endCsvWrite(); _uploading = false; }
    if (!_st->beginCsvWrite(_uploadName)) return "ERR:cannot begin upload";
    _uploading = true;
    _uploadBuf = "";
    return "UP_OK:" + _uploadName;
  }
  if (line.startsWith("END:")) {
    if (!_uploading) return "ERR:no upload started";
    _uploading = false;
    // 立即消化残余缓冲, 确保帧都写入
    if (_uploadBuf.length() > 0) {
      _st->appendCsvData(_uploadBuf);
      _uploadBuf = "";
    }
    _savePending = true;
    return "";
  }
  if (_uploading) {
    if (_uploadBuf.length() + line.length() > MAX_CSV_UPLOAD) {  // 内存上限 100KB
      return "ERR:buffer full";
    }
    _uploadBuf += line;
    return "";
  }
  return "ERR:no upload started";
}

// 串口路径: 同步直接写入 (不做内存缓冲, 避免串口高速涌入撑爆 RAM)
String BleService::handleUploadSync(const String& line) {
  if (line.startsWith("UP:")) {
    _uploadName = _st->sanitizeName(line.substring(3));
    if (_uploading) { _st->endCsvWrite(); _uploading = false; }
    if (!_st->beginCsvWrite(_uploadName)) return "ERR:cannot begin upload";
    _uploading = true;
    _uploadBuf = "";
    return "UP_OK:" + _uploadName;
  }
  if (line.startsWith("END:")) {
    if (!_uploading) return "ERR:no upload started";
    _uploading = false;
    _savePending = true;
    return "";
  }
  if (_uploading) {
    _st->appendCsvData(line + "\n");   // 串口行已 trim 去 \n, 补回
    return "";
  }
  return "ERR:no upload started";
}

String BleService::handleCommand(const String& cmd) {
  String c = cmd;
  c.trim();
  if (c == "PLAY") {
    _player->toggle();
    if (_player->state() == Player::IDLE) return "ERR:no preset";
    return "OK:" + _player->current();
  }
  if (c == "STOP") { _player->stop(); return "OK"; }
  if (c == "PAUSE") { _player->toggle(); return "OK"; }
  if (c == "NEXT") {
    String names[MAX_PRESETS];
    int n = _st->listPresets(names, MAX_PRESETS);
    if (n == 0) return "ERR:no presets";
    _player->next(names, n);
    return "OK:" + _player->current();
  }
  if (c == "LIST") {
    String names[MAX_PRESETS];
    int n = _st->listPresets(names, MAX_PRESETS);
    String out = "LIST:";
    for (int i = 0; i < n; i++) { if (i) out += ","; out += names[i]; }
    return out;
  }
  if (c.startsWith("FSINFO")) {
    return _st->fsInfo();
  }
  if (c.startsWith("BRIGHT:")) {
    int pct = c.substring(7).toInt();
    if (pct < 0 || pct > 100) return "ERR:range";
    _st->setBrightness((uint8_t)pct);
    _player->setBrightness((uint8_t)pct);
    _player->resendCurrent();
    return "OK:bright " + String(pct);
  }
  if (c.startsWith("LOAD:")) {
    String name = _st->sanitizeName(c.substring(5));
    if (!_st->presetExists(name)) return "ERR:not found";
    _player->play(name);
    return "OK:" + name;
  }
  if (c.startsWith("DELETE:")) {
    String name = _st->sanitizeName(c.substring(7));
    return _st->deletePreset(name) ? "OK:deleted" : "ERR:not found";
  }
  if (c == "RESET") {
    _st->eraseAll();
    _player->stop();
    return "OK:factory reset";
  }
  if (c == "STATUS") {
    String names[MAX_PRESETS];
    int n = _st->listPresets(names, MAX_PRESETS);
    const char* st = _player->state() == Player::PLAYING ? "playing"
                   : _player->state() == Player::PAUSED ? "paused" : "idle";
    String out = "STATUS:st=" + String(st) + ";cur=" + _player->current() +
                 ";n=" + String(n) + ";bright=" + String(_player->brightness());
    return out;
  }
  // ---- 调参 (串口/BLE/网页共用) ----
  if (c == "CFG" || c == "GETCFG") {
    String out = "CFG:baud=" + String(_st->getRfBaud()) +
                 ";pre=" + String(_st->getRfPreamble()) +
                 ";mode=" + String(_st->getRfMode()) +
                 ";inv=" + String(_st->getRfInvert() ? 1 : 0) +
                 ";bright=" + String(_st->getBrightness()) +
                 ";loop=" + String(_st->getLoopPlay() ? 1 : 0) +
                 ";ssid=" + _st->getStaSsid() +
                 ";ap=" + _st->getApPass();
    return out;
  }
  if (c.startsWith("RFBAUD:")) {
    long v = c.substring(7).toInt();
    if (v < 300 || v > 20000) return "ERR:range (300-20000)";
    _st->setRfBaud((uint32_t)v);
    _rf->setBaud((uint32_t)v);
    return "OK:baud " + String(v);
  }
  if (c.startsWith("RFPRE:")) {
    int v = c.substring(6).toInt();
    if (v < 0 || v > 32) return "ERR:range (0-32)";
    _st->setRfPreamble((uint8_t)v);
    _rf->setPreamble((uint8_t)v);
    return "OK:pre " + String(v);
  }
  if (c.startsWith("RFMODE:")) {
    int v = c.substring(7).toInt();
    if (v != 0 && v != 1) return "ERR:mode (0=NRZ 1=UART)";
    _st->setRfMode((uint8_t)v);
    _rf->setMode((RmtOok::Mode)v);
    return "OK:mode " + String(v);
  }
  if (c.startsWith("RFINV:")) {
    int v = c.substring(6).toInt();
    if (v != 0 && v != 1) return "ERR:inv (0/1)";
    _st->setRfInvert(v == 1);
    _rf->setInvert(v == 1);
    return "OK:inv " + String(v);
  }
  if (c.startsWith("LOOP:")) {
    int v = c.substring(5).toInt();
    if (v != 0 && v != 1) return "ERR:loop (0/1)";
    _st->setLoopPlay(v == 1);
    return "OK:loop " + String(v);
  }
  if (c.startsWith("SETAP:")) {
    String p = c.substring(6);
    if (p.length() < 8 || p.length() > 32) return "ERR:pass 8-32 chars";
    _st->setApPass(p);
    return "OK:ap pass saved (reboot to apply)";
  }
  if (c.startsWith("SETSTA:")) {
    // SETSTA:ssid;pass
    int semi = c.indexOf(';', 7);
    if (semi <= 7) return "ERR:format SETSTA:ssid;pass";
    String ssid = c.substring(7, semi);
    String pass = c.substring(semi + 1);
    if (ssid.length() == 0 || ssid.length() > 32) return "ERR:ssid 1-32 chars";
    _st->setStaSsid(ssid);
    _st->setStaPass(pass);
    return "OK:sta saved (reboot to apply)";
  }
  if (c == "REBOOT") {
    delay(100);
    ESP.restart();
    return "OK:rebooting";
  }
  // ---- 通用 OOK 场控帧直发 ----
  // RFC:<hex>         发送完整帧(校验字节由调用方附加)
  // RFR:/RFG:/RFB:/RFW: 全区常亮红/绿/蓝/白   RFOFF: 全区熄灭
  // RF:r,g,b          全区常亮指定色 (0-15)   RFF:r,g,b,f 带闪烁功能
  {
    String arg = c.substring(4);
    if (c.startsWith("RFC:")) {
      String hex = arg;
      hex.replace(" ", "");
      hex.replace(":", "");
      if (hex.length() == 0 || hex.length() % 2) return "ERR:hex";
      uint8_t buf[32];
      size_t n = hex.length() / 2;
      if (n > 24) return "ERR:too long";
      for (size_t i = 0; i < n; i++) {
        char hi = hex[i * 2], lo = hex[i * 2 + 1];
        auto nib = [](char ch) -> int {
          if (ch >= '0' && ch <= '9') return ch - '0';
          if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
          if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
          return -1;
        };
        int h = nib(hi), l = nib(lo);
        if (h < 0 || l < 0) return "ERR:hex";
        buf[i] = (uint8_t)((h << 4) | l);
      }
      _rf->sendFieldFrame(buf, n, 6, 250);
      return "OK:field";
    }
    if (c.startsWith("RFR:")) { _rf->sendFieldFrame((const uint8_t*)"\x00\xFF\xFF\xFF\x01\x00\x94", 7, 6, 250); return "OK:red"; }
    if (c.startsWith("RFG:")) { _rf->sendFieldFrame((const uint8_t*)"\x00\xFF\xFF\xFF\x01\x01\x95", 7, 6, 250); return "OK:green"; }
    if (c.startsWith("RFB:")) { _rf->sendFieldFrame((const uint8_t*)"\x00\xFF\xFF\xFF\x01\x02\x96", 7, 6, 250); return "OK:blue"; }
    if (c.startsWith("RFW:")) { _rf->sendFieldFrame((const uint8_t*)"\x00\xFF\xFF\xFF\x01\x04\x98", 7, 6, 250); return "OK:white"; }
    if (c.startsWith("RFOFF")) { _rf->sendFieldFrame((const uint8_t*)"\x00\xFF\xFF\xFF\x00\x00\x93", 7, 6, 250); return "OK:off"; }
    if (c.startsWith("RFINV:")) {
      // 诊断: 反相电平发射红色帧 (F113 DATA 极性测试)
      _rf->sendFieldFrameInv((const uint8_t*)"\x00\xFF\xFF\xFF\x01\x00\x94", 7, 6, 250);
      return "OK:inv-red";
    }
    if (c.startsWith("RF:")) {
      // RF:r,g,b[,f]  -- 全区 16 色(调色板) + 可选闪烁状态
      int c2 = arg.indexOf(',');
      int c3 = arg.indexOf(',', c2 + 1);
      if (c2 <= 0 || c3 <= 0) return "ERR:format r,g,b[,f]";
      int r = arg.substring(0, c2).toInt() & 0x0F;
      int g = arg.substring(c2 + 1, c3).toInt() & 0x0F;
      int b = arg.substring(c3 + 1).toInt() & 0x0F;
      int f = 1;
      int c4 = arg.indexOf(',', c3 + 1);
      if (c4 > 0) { f = arg.substring(c3 + 1, c4).toInt(); if (f < 0 || f > 6) f = 1; }
      uint8_t frame[7] = {0x00, 0xFF, 0xFF, 0xFF, (uint8_t)f, 0x00, 0x00};
      // 调色板映射: 16 色 LUT
      static const uint8_t PAL[16] = {0,1,2,3,4,5,6,7,8,9,0xA,0xB,0xC,0xD,0xE,0xF};
      // 直接按 4bit 调色板索引选择 (r,g,b 中任一个非零的候选):
      // 简化: 支持预定义色板索引 0-15 通过 r 传入, g/b 可给 0
      frame[5] = PAL[r & 0x0F];
      uint8_t ck = 0x96;
      for (int i = 0; i < 6; i++) ck = (uint8_t)(ck + frame[i]);
      frame[6] = ck;
      _rf->sendFieldFrame(frame, 7, 6, 250);
      return "OK:field-rgb";
    }
  }
  if (c.startsWith("CARRIER")) {
    int ms = c.length() > 7 ? c.substring(8).toInt() : 500;
    bool ok = _rf->carrier((uint32_t)ms);
    return ok ? "OK:carrier" : "ERR:carrier";
  }
  if (c.startsWith("GPIOH")) {
    // 直接拉高 GPIO3 (不走 RMT), 诊断接线
    int ms = c.length() > 5 ? c.substring(6).toInt() : 3000;
    _rf->release();
    pinMode(PIN_ASK_TX, OUTPUT);
    digitalWrite(PIN_ASK_TX, HIGH);
    int lv = digitalRead(PIN_ASK_TX);   // 读回, 确认是否真的高
    delay(ms);
    digitalWrite(PIN_ASK_TX, LOW);
#ifdef FIRMWARE_LOG
    Serial.printf("[gpio] PIN_ASK_TX=%d readback=%d\n", PIN_ASK_TX, lv);
#endif
    return lv ? "OK:gpioh-high" : "ERR:gpioh-low";
  }
  if (c.startsWith("COLOR:")) {
    // COLOR:0:15,15,0,0;1:2,15,8,0;...  (ch:func,r,g,b; 分号分隔)
    uint8_t payload[STREAM_PAYLOAD_LEN] = {0};
    String rest = c.substring(6);
    int from = 0;
    while (from < (int)rest.length()) {
      int semi = rest.indexOf(';', from);
      String item = (semi < 0) ? rest.substring(from) : rest.substring(from, semi);
      int colon = item.indexOf(':');
      if (colon > 0) {
        int ch = item.substring(0, colon).toInt();
        if (ch >= 0 && ch < 10) {
          int c2 = item.indexOf(',', colon + 1);
          int c3 = item.indexOf(',', c2 + 1);
          int c4 = item.indexOf(',', c3 + 1);
          if (c2 > 0 && c3 > 0 && c4 > 0) {
            int func = item.substring(colon + 1, c2).toInt() & 0x0F;
            int r = item.substring(c2 + 1, c3).toInt() & 0x0F;
            int g = item.substring(c3 + 1, c4).toInt() & 0x0F;
            int b = item.substring(c4 + 1).toInt() & 0x0F;
            payload[ch * 2]     = (uint8_t)((func << 4) | r);
            payload[ch * 2 + 1] = (uint8_t)((g << 4) | b);
          }
        }
      }
      if (semi < 0) break;
      from = semi + 1;
    }
    _player->sendPayload(payload);
    return "OK:color";
  }
  return "ERR:unknown";
}
