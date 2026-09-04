#include "storage.h"
#include "protocol.h"
#include <LittleFS.h>

#define NS "lumac3"

bool Storage::begin() {
  _pref.begin(NS, false);
  _fsReady = LittleFS.begin(true);
  if (_fsReady) {
    if (!LittleFS.exists("/seq")) LittleFS.mkdir("/seq");
  }
  return _fsReady;
}

uint32_t Storage::getRfBaud()     { return _pref.getUInt("rf_baud", RF_BAUD_DEFAULT); }
void Storage::setRfBaud(uint32_t v){ _pref.putUInt("rf_baud", v); }
uint8_t Storage::getRfPreamble()  { return _pref.getUChar("rf_pre", RF_PREAMBLE_DEFAULT); }
void Storage::setRfPreamble(uint8_t v){ _pref.putUChar("rf_pre", v); }
uint8_t Storage::getRfMode()      { return _pref.getUChar("rf_mode", RF_MODE_DEFAULT); }
void Storage::setRfMode(uint8_t v){ _pref.putUChar("rf_mode", v); }
bool Storage::getRfInvert()       { return _pref.getBool("rf_inv", RF_INVERT_DEFAULT); }
void Storage::setRfInvert(bool v) { _pref.putBool("rf_inv", v); }
uint8_t Storage::getBrightness()  { return _pref.getUChar("bright", 100); }
void Storage::setBrightness(uint8_t pct) { if (pct > 100) pct = 100; _pref.putUChar("bright", pct); }
bool Storage::getLoopPlay()       { return _pref.getBool("loop", true); }
void Storage::setLoopPlay(bool v) { _pref.putBool("loop", v); }
String Storage::getCurrentPreset(){ return _pref.getString("cur_pre", ""); }
void Storage::setCurrentPreset(const String& n){ _pref.putString("cur_pre", n); }
String Storage::getApPass()       { return _pref.getString("ap_pass", WIFI_AP_PASS_DEFAULT); }
void Storage::setApPass(const String& v){ _pref.putString("ap_pass", v); }
String Storage::getStaSsid()      { return _pref.getString("sta_ssid", ""); }
void Storage::setStaSsid(const String& v){ _pref.putString("sta_ssid", v); }
String Storage::getStaPass()      { return _pref.getString("sta_pass", ""); }
void Storage::setStaPass(const String& v){ _pref.putString("sta_pass", v); }

String Storage::sanitizeName(const String& name) {
  String n = name;
  n.trim();
  n.replace("/", "_");
  n.replace("\x5C", "_");
  n.replace(".", "_");
  n.replace(" ", "_");
  if (n.length() == 0) n = "show";
  if (n.length() > MAX_UPLOAD_NAME) n = n.substring(0, MAX_UPLOAD_NAME);
  return n;
}

bool Storage::presetExists(const String& name) {
  if (!_fsReady) return false;
  return LittleFS.exists("/seq/" + sanitizeName(name) + ".bin");
}

// 从 CSV 表头定位各列
static int findCol(const String& header, const char* key) {
  int pos = 0, idx = 0;
  while (pos <= (int)header.length()) {
    int comma = header.indexOf(',', pos);
    String col = (comma < 0) ? header.substring(pos) : header.substring(pos, comma);
    col.trim();
    if (col == key) return idx;
    idx++;
    if (comma < 0) break;
    pos = comma + 1;
  }
  return -1;
}

bool Storage::csvLineToPayload(const String& line, const String& header, uint8_t* payload20) {
  // 找到 ch0..ch9 的 function/red/green/blue 列索引
  int colFunc[10], colR[10], colG[10], colB[10];
  for (int i = 0; i < 10; i++) {
    String p = String("ch") + i;
    colFunc[i] = findCol(header, (p + "_function").c_str());
    colR[i]    = findCol(header, (p + "_red").c_str());
    colG[i]    = findCol(header, (p + "_green").c_str());
    colB[i]    = findCol(header, (p + "_blue").c_str());
    if (colFunc[i] < 0 || colR[i] < 0 || colG[i] < 0 || colB[i] < 0) return false;
  }
  // 拆行
  String cells[64];
  int n = 0, pos = 0;
  while (pos <= (int)line.length() && n < 64) {
    int comma = line.indexOf(',', pos);
    cells[n++] = (comma < 0) ? line.substring(pos) : line.substring(pos, comma);
    if (comma < 0) break;
    pos = comma + 1;
  }
  auto cell = [&](int colIdx) -> int {
    if (colIdx < 0 || colIdx >= n) return 0;
    return cells[colIdx].toInt();
  };
  for (int i = 0; i < 10; i++) {
    int func = cell(colFunc[i]) & 0x0F;
    int r    = cell(colR[i])    & 0x0F;
    int g    = cell(colG[i])    & 0x0F;
    int b    = cell(colB[i])    & 0x0F;
    payload20[i * 2]     = (uint8_t)((func << 4) | r);
    payload20[i * 2 + 1] = (uint8_t)((g << 4) | b);
  }
  return true;
}

String Storage::saveCsvAsPreset(const String& name, const String& csvText) {
  if (!_fsReady) return "LittleFS not ready";
  if (csvText.length() > MAX_CSV_UPLOAD) return "CSV too large";
  String safe = sanitizeName(name);

  // 边解析边写文件 (避免大数组占用 RAM)
  File f = LittleFS.open("/seq/" + safe + ".bin", "w");
  if (!f) return "cannot create file";
  uint8_t magic[4] = { 'L', 'F', 'C', '3' };
  f.write(magic, 4);
  uint16_t countPlaceholder = 0;
  f.write((uint8_t*)&countPlaceholder, 2);

  String header;
  uint8_t payload[STREAM_PAYLOAD_LEN];
  uint16_t count = 0;
  int lineStart = 0;
  while (lineStart <= (int)csvText.length() && count < MAX_CSV_FRAMES) {
    int nl = csvText.indexOf('\n', lineStart);
    String line = (nl < 0) ? csvText.substring(lineStart) : csvText.substring(lineStart, nl);
    line.trim();
    if (line.length() == 0) { if (nl < 0) break; lineStart = nl + 1; continue; }
    if (line.startsWith("#")) { if (nl < 0) break; lineStart = nl + 1; continue; }
    if (header.length() == 0) {
      header = line;
      if (nl < 0) break;
      lineStart = nl + 1;
      continue;
    }
    int tcol = findCol(header, "frame_time_ms");
    if (tcol < 0) { f.close(); return "missing frame_time_ms column"; }
    String cells[64];
    int n = 0, pos = 0;
    while (pos <= (int)line.length() && n < 64) {
      int comma = line.indexOf(',', pos);
      cells[n++] = (comma < 0) ? line.substring(pos) : line.substring(pos, comma);
      if (comma < 0) break;
      pos = comma + 1;
    }
    if (tcol >= n) { if (nl < 0) break; lineStart = nl + 1; continue; }
    if (!csvLineToPayload(line, header, payload)) { if (nl < 0) break; lineStart = nl + 1; continue; }
    uint32_t t = (uint32_t)cells[tcol].toInt();
    f.write((uint8_t*)&t, 4);
    f.write(payload, STREAM_PAYLOAD_LEN);
    count++;
    if (nl < 0) break;
    lineStart = nl + 1;
  }
  f.seek(4);
  f.write((uint8_t*)&count, 2);
  f.close();
  if (count == 0) {
    LittleFS.remove("/seq/" + safe + ".bin");
    return "no frames parsed";
  }
  setCurrentPreset(safe);
  return "";
}

bool Storage::loadPreset(const String& name, File& out) {
  if (!_fsReady) return false;
  out = LittleFS.open("/seq/" + sanitizeName(name) + ".bin", "r");
  if (!out) return false;
  uint8_t magic[4];
  if (out.read(magic, 4) != 4 || memcmp(magic, "LFC3", 4) != 0) { out.close(); return false; }
  return true;
}

// ================= 流式 CSV 写入 =================
bool Storage::beginCsvWrite(const String& name) {
  if (!_fsReady) return false;
  if (_csvWriteActive) endCsvWrite();
  String safe = sanitizeName(name);
  _csvFile = LittleFS.open("/seq/" + safe + ".bin", "w");
  if (!_csvFile) return false;
  uint8_t magic[4] = { 'L', 'F', 'C', '3' };
  uint16_t countPlaceholder = 0;
  _csvFile.write(magic, 4);
  _csvFile.write((uint8_t*)&countPlaceholder, 2);
  _csvHeader = "";
  _csvLineBuf = "";
  _csvCount = 0;
  _csvWriteActive = true;
  return true;
}

bool Storage::appendCsvData(const String& chunk) {
  if (!_csvWriteActive || !_csvFile) return false;
  _csvLineBuf += chunk;
  while (true) {
    int nl = _csvLineBuf.indexOf('\n');
    if (nl < 0) break;
    String line = _csvLineBuf.substring(0, nl);
    _csvLineBuf = _csvLineBuf.substring(nl + 1);
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;
    if (_csvHeader.length() == 0) { _csvHeader = line;
#ifdef FIRMWARE_LOG
      Serial.printf("[csv] header len=%u\n", (unsigned)_csvHeader.length());
#endif
      continue; }
    if (_csvCount >= MAX_CSV_FRAMES) break;
    uint8_t payload[STREAM_PAYLOAD_LEN];
    if (!csvLineToPayload(line, _csvHeader, payload)) {
#ifdef FIRMWARE_LOG
      if (_csvCount == 0) {
        static bool warned = false;
        if (!warned) { warned = true; Serial.println("[csv] parse fail first data line"); }
      }
#endif
      continue; }
    int tcol = findCol(_csvHeader, "frame_time_ms");
    if (tcol < 0) { _csvHeader = ""; continue; }
    String cells[64];
    int n = 0, pos = 0;
    while (pos <= (int)line.length() && n < 64) {
      int comma = line.indexOf(',', pos);
      cells[n++] = (comma < 0) ? line.substring(pos) : line.substring(pos, comma);
      if (comma < 0) break;
      pos = comma + 1;
    }
    if (tcol >= n) continue;
    uint32_t t = (uint32_t)cells[tcol].toInt();
    _csvFile.write((uint8_t*)&t, 4);
    _csvFile.write(payload, STREAM_PAYLOAD_LEN);
    _csvCount++;
    if ((_csvCount & 0x3F) == 0) yield();   // 每 64 帧喂一次看门狗
  }
  return true;
}

String Storage::endCsvWrite() {
  if (!_csvWriteActive) return "no active upload";
  _csvWriteActive = false;
  if (!_csvFile) return "file not open";
  String path = String("/seq/") + _csvFile.name();
  int fnLen = path.length();
  (void)fnLen;
  _csvFile.seek(4);
  _csvFile.write((uint8_t*)&_csvCount, 2);
  _csvFile.close();
  if (_csvCount == 0) {
    LittleFS.remove(path);
    return "no frames parsed";
  }
  int slash = path.lastIndexOf('/');
  String safe = (slash >= 0) ? path.substring(slash + 1) : path;
  if (safe.endsWith(".bin")) safe = safe.substring(0, safe.length() - 4);
  setCurrentPreset(safe);
  return "";
}

int Storage::listPresets(String* names, int max) {
  if (!_fsReady) return 0;
  File root = LittleFS.open("/seq");
  if (!root || !root.isDirectory()) return 0;
  int n = 0;
  File f = root.openNextFile();
  while (f && n < max) {
    String fn = String(f.name());
    int slash = fn.lastIndexOf('/');
    if (slash >= 0) fn = fn.substring(slash + 1);
    if (fn.endsWith(".bin")) {
      names[n++] = fn.substring(0, fn.length() - 4);  // 去掉 .bin
    }
    f = root.openNextFile();
  }
  return n;
}

bool Storage::deletePreset(const String& name) {
  if (!_fsReady) return false;
  return LittleFS.remove("/seq/" + sanitizeName(name) + ".bin");
}

String Storage::fsInfo() {
  if (!_fsReady) return "FSINFO:littlefs-not-ready";
  File root = LittleFS.open("/seq");
  if (!root || !root.isDirectory()) return "FSINFO:no-seq-dir";
  String out = "FSINFO:";
  File f = root.openNextFile();
  while (f) {
    if (f.isDirectory()) { f = root.openNextFile(); continue; }
    String fn = String(f.name());
    out += fn + "=" + String(f.size()) + ";";
    f = root.openNextFile();
  }
  if (out == "FSINFO:") out = "FSINFO:empty";
  return out;
}

void Storage::eraseAll() {
  if (_fsReady) {
    File root = LittleFS.open("/seq");
    if (root && root.isDirectory()) {
      File f = root.openNextFile();
      while (f) {
        LittleFS.remove(String("/seq/") + String(f.name()));
        f = root.openNextFile();
      }
    }
  }
  _pref.clear();
}
