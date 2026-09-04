#include "wifi_ap.h"
#include "webui.h"

void WifiAp::begin(Storage* storage, Player* player,
                   void (*forwardFn)(uint8_t, const uint8_t*, uint8_t)) {
  _st = storage;
  _player = player;
  _forward = forwardFn;

  String apPass = _st->getApPass();
  String staSsid = _st->getStaSsid();
  String staPass = _st->getStaPass();

  if (staSsid.length() > 0) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  } else {
    WiFi.mode(WIFI_AP);
  }
  WiFi.softAP(WIFI_AP_SSID_DEFAULT, apPass.c_str());
  _apIp = WiFi.softAPIP().toString();

  _server.on("/", HTTP_GET, [this]() { handleRoot(); });
  _server.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
  _server.on("/api/list", HTTP_GET, [this]() { handleList(); });
  _server.on("/api/cmd", HTTP_POST, [this]() { handleCmd(); });
  _server.on("/api/delete", HTTP_POST, [this]() { handleDelete(); });
  _server.on("/api/wifi", HTTP_GET, [this]() {
    String out = "{\"ssid\":\"" + _st->getStaSsid() + "\"}";
    _server.send(200, "application/json", out);
  });
  _server.on("/api/wifi", HTTP_POST, [this]() { handleWifi(); });
  _server.on("/api/reboot", HTTP_POST, [this]() { _server.send(200, "text/plain", "rebooting"); ESP.restart(); });
  _server.on("/api/upload", HTTP_POST, [this]() { handleUploadStart(); },
             [this]() { handleUploadData(); });
  _server.begin();

  _udp.begin(UDP_PORT);
}

void WifiAp::handleRoot() {
  _server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void WifiAp::handleStatus() {
  String names[MAX_PRESETS];
  int n = _st->listPresets(names, MAX_PRESETS);
  const char* st = _player->state() == Player::PLAYING ? "playing"
                 : _player->state() == Player::PAUSED ? "paused" : "idle";
  _staIp = WiFi.localIP().toString();
  String out = "{\"state\":\"" + String(st) + "\",\"current\":\"" + _player->current() +
               "\",\"bright\":" + String(_player->brightness()) +
               ",\"presets\":" + String(n) +
               ",\"rf_baud\":" + String(_st->getRfBaud()) +
               ",\"rf_pre\":" + String(_st->getRfPreamble()) +
               ",\"rf_mode\":" + String(_st->getRfMode()) +
               ",\"rf_inv\":" + String(_st->getRfInvert() ? 1 : 0) +
               ",\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? _staIp : "") + "\"}";
  _server.send(200, "application/json", out);
}

void WifiAp::handleList() {
  String names[MAX_PRESETS];
  int n = _st->listPresets(names, MAX_PRESETS);
  String out;
  for (int i = 0; i < n; i++) { if (i) out += ","; out += names[i]; }
  _server.send(200, "text/plain", out);
}

void WifiAp::handleCmd() {
  String body = _server.arg("plain");
  body.trim();
  // 委托给注入的命令处理器 (与 BLE/串口同一套命令)
  String reply = _cmdFn ? _cmdFn(body) : "ERR:no cmd handler";
  _server.send(200, "text/plain", reply);
  if (_notify) _notify(reply.c_str());
}

void WifiAp::handleDelete() {
  String body = _server.arg("plain");
  body.trim();
  String name = _st->sanitizeName(body);
  _server.send(200, "text/plain", _st->deletePreset(name) ? "OK:deleted" : "ERR:not found");
}

void WifiAp::handleWifi() {
  String body = _server.arg("plain");
  // 简易解析 {"ssid":"..","pass":".."}
  int s = body.indexOf("\"ssid\":\"");
  if (s < 0) { _server.send(400, "text/plain", "bad json"); return; }
  s += 8;                       // "ssid":" 共 8 字符
  int e = body.indexOf('\"', s);
  String ssid = body.substring(s, e);
  s = body.indexOf("\"pass\":\"", e);
  if (s < 0) { _server.send(400, "text/plain", "bad json"); return; }
  s += 7;                       // "pass":" 共 7 字符
  e = body.indexOf('\"', s);
  String pass = body.substring(s, e);
  _st->setStaSsid(ssid);
  _st->setStaPass(pass);
  _server.send(200, "text/plain", "OK:saved, rebooting...");
}

void WifiAp::handleUploadStart() {
  _uploadName = _server.arg("name");
  if (_uploadName.length() == 0) _uploadName = "show";
  _server.sendHeader("Connection", "close");
  _server.send(200, "text/plain", "uploading...");
}

void WifiAp::handleUploadData() {
  HTTPUpload& up = _server.upload();
  if (up.status == UPLOAD_FILE_START) {
    _uploadName = _server.arg("name");
    if (_uploadName.length() == 0) _uploadName = "show";
    if (!_st->beginCsvWrite(_uploadName)) {
      _server.send(500, "text/plain", "ERR:cannot begin upload");
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    String chunk((char*)up.buf, up.currentSize);
    _st->appendCsvData(chunk);
  } else if (up.status == UPLOAD_FILE_END) {
    String err = _st->endCsvWrite();
    String msg = err.length() ? ("ERR:" + err) : ("OK:preset " + _uploadName + " saved");
    if (_notify) _notify(msg.c_str());
  }
}

void WifiAp::pollUdp() {
  if (WiFi.status() != WL_CONNECTED) { _staConnected = false; return; }
  _staConnected = true;
  int n = _udp.parsePacket();
  if (n > 0 && n <= TLV_MAX_LEN) {
    uint8_t buf[TLV_MAX_LEN];
    int rd = _udp.read(buf, n);
    if (rd >= 6 && looks_like_tlv(buf, rd)) {
      // 逐字节喂解析器
      static TlvParser* parser = nullptr;
      if (!parser) {
        parser = new TlvParser();
        parser->setCallback(_forward);
      }
      for (int i = 0; i < rd; i++) parser->feed(buf[i]);
    }
  }
}

void WifiAp::update() {
  _server.handleClient();
  pollUdp();
}
