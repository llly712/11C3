#pragma once
#include "config.h"
#include <Arduino.h>
#include <functional>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include "player.h"
#include "storage.h"
#include "protocol.h"

// WiFi AP + HTTP 控制台 + UDP 32712 下位机监听 (STA 模式)
class WifiAp {
public:
  void begin(Storage* storage, Player* player,
             void (*forwardFn)(uint8_t cmd, const uint8_t* payload, uint8_t plen));
  void update();
  bool staConnected() const { return _staConnected; }
  String apIp() const { return _apIp; }
  String staIp() const { return _staIp; }
  void setNotifyFn(void (*fn)(const char*)) { _notify = fn; }
  // 文本命令处理器 (由 .ino 注入, 复用 BLE 命令实现)
  void setCmdFn(std::function<String(const String&)> fn) { _cmdFn = fn; }

private:
  Storage* _st = nullptr;
  Player*  _player = nullptr;
  void (*_forward)(uint8_t, const uint8_t*, uint8_t) = nullptr;
  void (*_notify)(const char*) = nullptr;
  std::function<String(const String&)> _cmdFn;
  WebServer _server{80};
  WiFiUDP _udp;
  bool _staConnected = false;
  String _apIp, _staIp;
  String _uploadName;
  String _lastCmd;
  unsigned long _lastCmdAt = 0;

  void handleRoot();
  void handleStatus();
  void handleList();
  void handleCmd();
  void handleUploadStart();
  void handleUploadData();
  void handleDelete();
  void handleWifi();
  void handleReboot();
  void pollUdp();
};
