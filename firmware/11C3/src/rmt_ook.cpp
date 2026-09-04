#include "rmt_ook.h"
#include "config.h"

bool RmtOok::begin(gpio_num_t pin) {
  if (_chan) return true;
  rmt_tx_channel_config_t cfg = {};
  cfg.gpio_num = pin;
  cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  cfg.resolution_hz = 1000000;           // 1 tick = 1us
  cfg.mem_block_symbols = 64;             // 64 words (2 blocks); C3 RMT 内存有限, 大帧需分段发送
  cfg.trans_queue_depth = 4;
  cfg.flags.invert_out = false;
  esp_err_t er = rmt_new_tx_channel(&cfg, &_chan);
#ifdef FIRMWARE_LOG
  Serial.printf("[rf] begin(pin=%d) rmt_new_tx_channel=%s\r\n", pin, esp_err_to_name(er));
#endif
  if (er != ESP_OK) return false;

  rmt_copy_encoder_config_t ecfg = {};
  er = rmt_new_copy_encoder(&ecfg, &_enc);
#ifdef FIRMWARE_LOG
  Serial.printf("[rf] rmt_new_copy_encoder=%s\r\n", esp_err_to_name(er));
#endif
  if (er != ESP_OK) return false;
  er = rmt_enable(_chan);
#ifdef FIRMWARE_LOG
  Serial.printf("[rf] rmt_enable=%s\r\n", esp_err_to_name(er));
#endif
  if (er != ESP_OK) return false;
  return true;
}

void RmtOok::setBaud(uint32_t baud)  { if (baud >= 300 && baud <= 200000) _baud = baud; }
void RmtOok::setMode(Mode m)         { _mode = m; }
void RmtOok::setInvert(bool inv)     { _invert = inv; }
void RmtOok::setPreamble(uint8_t n)  { _preamble = n; }
bool RmtOok::busy() const            { return _busy; }

// EV1527/PT2262 类 PWM 编码发射
// 时序(单位 tick, 1 tick=1us): 同步码 = 4T 低 + 124T 高; 每个数据位 = 4T 高 + (bit?4T:12T) 低
// 24 位: 20 位地址(MSB) + 4 位按键码(LSB)
bool RmtOok::sendEv1527(uint32_t code24, uint8_t repeat, uint32_t tUs) {
  if (!_chan || !_enc) return false;
  uint32_t T = tUs;
  if (T < 50 || T > 5000) return false;
  static rmt_symbol_word_t syms[64];
  size_t n = 0;
  // 同步码: 4T 低 + 124T 高
  syms[n].duration0 = 4 * T;   syms[n].level0 = 0;
  syms[n].duration1 = 124 * T; syms[n].level1 = 1;
  n++;
  // 24 位数据, MSB 优先
  for (int i = 23; i >= 0; i--) {
    bool bit = (code24 >> i) & 1;
    syms[n].duration0 = 4 * T;   syms[n].level0 = 1;
    syms[n].duration1 = (bit ? 4 : 12) * T; syms[n].level1 = 0;
    n++;
  }
  rmt_transmit_config_t tcfg = {};
  tcfg.loop_count = 0;
  tcfg.flags.eot_level = 0;   // 空闲无载波
  bool ok = true;
  for (uint8_t r = 0; r < repeat; r++) {
    esp_err_t err = rmt_transmit(_chan, _enc, syms, n * sizeof(rmt_symbol_word_t), &tcfg);
    if (err == ESP_OK) {
      rmt_tx_wait_all_done(_chan, portMAX_DELAY);
    } else {
      ok = false;
      break;
    }
    delay(4);   // 帧间间隔, 让接收端识别重复帧
  }
  return ok;
}

// 把比特序列编码成 RMT symbol word (每 word 2 个半符号: level0/dur0 + level1/dur1)
// 返回写入的 word 数
static size_t bits_to_symbols(const bool* bits, size_t nbits, uint32_t ticks_per_bit,
                              bool invert, rmt_symbol_word_t* out, size_t cap) {
  size_t n = 0;
  for (size_t i = 0; i < nbits; i += 2) {
    if (n >= cap) break;
    bool b0 = bits[i], b1 = (i + 1 < nbits) ? bits[i + 1] : false;
    rmt_symbol_word_t w = {};
    w.duration0 = ticks_per_bit;
    w.level0    = (b0 ^ invert) ? 1 : 0;
    w.duration1 = ticks_per_bit;
    w.level1    = (b1 ^ invert) ? 1 : 0;
    out[n++] = w;
  }
  return n;
}

// 自定义 OOK 场控帧发射 (应用于派对/演出灯光控制器一类的 433MHz 主控通信)
// 物理层: 载波 433.920MHz ASK/OOK; 编码: 同步前缀 11111111000011110 + 每数据位三倍展开 "01"+d
// 每 bit 250us; 每字节 MSB first; 起始电平=1(载波开); 校验字节由调用方附加
// 释放 RMT 通道: 恢复 GPIO 为普通模式 (诊断用)
void RmtOok::release() {
  if (_chan) { rmt_disable(_chan); rmt_del_channel(_chan); _chan = nullptr; }
  if (_enc) { rmt_del_encoder(_enc); _enc = nullptr; }
}

// 连续载波诊断: 高电平持续 durationMs, 验证 F113 模块真的在发射
bool RmtOok::carrier(uint32_t durationMs) {
  if (!_chan || !_enc) return false;
  if (durationMs < 10 || durationMs > 10000) return false;
  // 拆成 50ms 一段 (50000 ticks), 避免单 symbol 超 RMT 内存限制
  const uint32_t segUs = 50000;
  rmt_symbol_word_t s = {};
  s.duration0 = segUs;
  s.level0    = 1;
  rmt_transmit_config_t tcfg = {};
  tcfg.loop_count = 0;
  tcfg.flags.eot_level = 0;
  uint32_t segs = (durationMs * 1000 + segUs - 1) / segUs;
  for (uint32_t i = 0; i < segs; i++) {
    esp_err_t err = rmt_transmit(_chan, _enc, &s, sizeof(s), &tcfg);
    if (err != ESP_OK) return false;
    rmt_tx_wait_all_done(_chan, portMAX_DELAY);
  }
  return true;
}

bool RmtOok::sendFieldFrame(const uint8_t* data, size_t len, uint8_t repeat, uint32_t bitUs) {
  if (!_chan || !_enc || !data || len == 0) return false;
  if (bitUs < 50 || bitUs > 5000) return false;
  const char* prefix = "11111111000011110";
  const size_t prefixLen = 17;
  // 总空中 bit = 前缀17 + len*8*3
  const size_t totalBits = prefixLen + len * 8 * 3;
  static bool air_bits[1024];          // 场控帧(21B) 最大 521 bits
  size_t k = 0;
  for (size_t i = 0; i < prefixLen && k < 1024; i++) air_bits[k++] = (prefix[i] == '1');
  for (size_t i = 0; i < len && k + 24 <= 1024; i++) {
    uint8_t v = data[i];
    for (int b = 7; b >= 0; b--) {
      uint8_t d = (v >> b) & 1;
      air_bits[k++] = 0;   // 展开位 '0'
      air_bits[k++] = 1;   // 展开位 '1'
      air_bits[k++] = d;   // 数据位
    }
  }
  if (k == 0) return false;
  // 每 2 个空中位打包进 1 个 symbol word, 大幅减少 RMT 内存占用
  static rmt_symbol_word_t syms[1024];
  size_t n = bits_to_symbols(air_bits, k, bitUs, false, syms, 1024);
  if (n == 0) return false;
  rmt_transmit_config_t tcfg = {};
  tcfg.loop_count = 0;
  tcfg.flags.eot_level = 0;   // 空闲无载波
  // 分段传输: 每段最多 48 words (mem_block_symbols=64 安全余量), 连续 queue 保持波形连续
  const size_t chunk = 48;
  bool ok = true;
  for (uint8_t r = 0; r < repeat; r++) {
    size_t off = 0;
    while (off < n) {
      size_t cnt = (n - off < chunk) ? (n - off) : chunk;
      esp_err_t err = rmt_transmit(_chan, _enc, &syms[off], cnt * sizeof(rmt_symbol_word_t), &tcfg);
      if (err != ESP_OK) { ok = false; break; }
      off += cnt;
      if (off >= n) rmt_tx_wait_all_done(_chan, portMAX_DELAY);
    }
    if (!ok) break;
    if (r + 1 < repeat) delay(20);   // 帧间约 20ms 低电平
  }
#ifdef FIRMWARE_LOG
  Serial.printf("[rf] field len=%u bits=%u us=%u rep=%u ok=%d\r\n", len, totalBits, bitUs, repeat, ok);
#endif
  return ok;
}

bool RmtOok::sendFieldFrameInv(const uint8_t* data, size_t len, uint8_t repeat, uint32_t bitUs) {
  if (!_chan || !_enc || !data || len == 0) return false;
  if (bitUs < 50 || bitUs > 5000) return false;
  const char* prefix = "11111111000011110";
  const size_t prefixLen = 17;
  const size_t totalBits = prefixLen + len * 8 * 3;
  static bool air_bits[1024];
  size_t k = 0;
  for (size_t i = 0; i < prefixLen && k < 1024; i++) air_bits[k++] = (prefix[i] == '0');  // 反相
  for (size_t i = 0; i < len && k + 24 <= 1024; i++) {
    uint8_t v = data[i];
    for (int b = 7; b >= 0; b--) {
      uint8_t d = (v >> b) & 1;
      air_bits[k++] = 1;   // '0' 反相
      air_bits[k++] = 0;   // '1' 反相
      air_bits[k++] = (d ^ 1);
    }
  }
  if (k == 0) return false;
  static rmt_symbol_word_t syms[1024];
  size_t n = bits_to_symbols(air_bits, k, bitUs, false, syms, 1024);
  if (n == 0) return false;
  rmt_transmit_config_t tcfg = {};
  tcfg.loop_count = 0;
  tcfg.flags.eot_level = 0;
  const size_t chunk = 48;
  bool ok = true;
  for (uint8_t r = 0; r < repeat; r++) {
    size_t off = 0;
    while (off < n) {
      size_t cnt = (n - off < chunk) ? (n - off) : chunk;
      esp_err_t err = rmt_transmit(_chan, _enc, &syms[off], cnt * sizeof(rmt_symbol_word_t), &tcfg);
      if (err != ESP_OK) { ok = false; break; }
      off += cnt;
      if (off >= n) rmt_tx_wait_all_done(_chan, portMAX_DELAY);
    }
    if (!ok) break;
    if (r + 1 < repeat) delay(20);
  }
#ifdef FIRMWARE_LOG
  Serial.printf("[rf] field_inv len=%u bits=%u us=%u rep=%u ok=%d\r\n", len, totalBits, bitUs, repeat, ok);
#endif
  return ok;
}

// 循环左移 16 位值
static uint16_t rol16(uint16_t v, uint8_t amount) {
  amount &= 15;
  if (amount == 0) return v;
  return (uint16_t)((v << amount) | (v >> (16 - amount)));
}

// 场控帧: 20 字节 STREAM payload(10通道) -> 21 字节帧 (槽0-8 对应 ch0-8, ch9 忽略)
// 每通道 [func<<4|r, g<<4|b], 槽值 = ROL16(FFFF RRRR GGGG BBBB, 2) 大端, phase 独立字节, 尾校验
static void build_field_d8(const uint8_t payload20[20], uint8_t phase, uint8_t out[21]) {
  out[0] = 0xD8;
  for (int ch = 0; ch < 9; ch++) {
    uint8_t a = payload20[ch * 2], b = payload20[ch * 2 + 1];
    uint8_t func = (a >> 4) & 0x0F;
    uint8_t r = a & 0x0F, g = (b >> 4) & 0x0F, bl = b & 0x0F;
    if (func > 3) func = 3;
    uint16_t w = rol16((uint16_t)((func << 12) | (r << 8) | (g << 4) | bl), 2);
    out[1 + ch * 2]     = (uint8_t)(w >> 8);
    out[1 + ch * 2 + 1] = (uint8_t)(w & 0xFF);
  }
  out[19] = phase;
  uint8_t ck = 0x96;
  for (int i = 0; i < 20; i++) ck = (uint8_t)(ck + out[i]);
  out[20] = ck;
}

// 按 phase 序列 (2,1,0,2,1,0) 连发 6 个帧, 帧间低电平 ~850us
// (对齐现场节奏: 131.1ms 帧起始间隔 - 130.25ms 帧时长)
bool RmtOok::sendFieldD8(const uint8_t payload20[20]) {
  if (!payload20) return false;
  static const uint8_t phases[6] = {2, 1, 0, 2, 1, 0};
  bool ok = true;
  for (int i = 0; i < 6; i++) {
    uint8_t frame[21];
    build_field_d8(payload20, phases[i], frame);
    if (!sendFieldFrame(frame, 21, 1, 250)) {
      ok = false;
      break;
    }
    if (i + 1 < 6) delayMicroseconds(850);   // 帧起始间隔约 131.1ms
  }
  return ok;
}

bool RmtOok::send(const uint8_t* data, size_t len) {
  if (!_chan || !_enc || !data || len == 0) return false;
  if (_busy) return false;

  uint32_t ticks = (uint32_t)(1000000ULL / _baud);   // 每 bit 的 tick 数
  if (ticks < 2) ticks = 2;
  const size_t max_bits = 128 * 16;                  // 最多处理 128 字节的位
  static bool bits[max_bits];
  static rmt_symbol_word_t syms[1024];
  size_t nbits = 0;

  auto pushBits = [&](const uint8_t* bytes, size_t n, bool lsbFirst) {
    for (size_t i = 0; i < n && nbits < max_bits; i++) {
      uint8_t v = bytes[i];
      for (int b = 0; b < 8; b++) {
        bits[nbits++] = lsbFirst ? ((v >> b) & 1) : ((v >> (7 - b)) & 1);
      }
    }
  };

  // 前导: 连续 1
  {
    static uint8_t ff[64];
    memset(ff, 0xFF, sizeof(ff));
    uint8_t n = _preamble > 64 ? 64 : _preamble;
    pushBits(ff, n, true);
  }

  if (_mode == MODE_UART) {
    // 每字节: 起始0 + 8bit(LSB) + 停止1
    for (size_t i = 0; i < len && nbits + 10 <= max_bits; i++) {
      bits[nbits++] = false;                        // start
      uint8_t v = data[i];
      for (int b = 0; b < 8; b++) bits[nbits++] = (v >> b) & 1;
      bits[nbits++] = true;                         // stop
    }
  } else {
    pushBits(data, len, true);                      // NRZ LSB first
  }

  size_t nwords = bits_to_symbols(bits, nbits, ticks, _invert, syms, 1024);
  if (nwords == 0) return false;

  _busy = true;
  rmt_transmit_config_t tcfg = {};
  tcfg.loop_count = 0;
  // 空闲电平: 反相后为低(无载波)
  tcfg.flags.eot_level = _invert ? 1 : 0;
  esp_err_t err = rmt_transmit(_chan, _enc, syms, nwords * sizeof(rmt_symbol_word_t), &tcfg);
#ifdef FIRMWARE_LOG
  Serial.printf("[rf] send nbits=%u nwords=%u baud=%u err=0x%02X\r\n", nbits, nwords, _baud, err);
#endif
  if (err == ESP_OK) {
    rmt_tx_wait_all_done(_chan, portMAX_DELAY);
  }
  _busy = false;
  return err == ESP_OK;
}
