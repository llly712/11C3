#pragma once
#include "config.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include "protocol.h"
#include "player.h"
#include "storage.h"
#include "rmt_ook.h"

// BLE 服务:
//  1) LumaFlow 兼容服务 7e570001/7e570002 —— 电脑 LumaFlow 可直接连接本设备当 BLE 发射器
//  2) 控制/上传服务 c3a50001..04 —— 手机 App/网页: 命令、CSV 上传、状态通知
class BleService {
public:
  void begin(Storage* storage, Player* player, RmtOok* rf,
             void (*forwardFn)(uint8_t cmd, const uint8_t* payload, uint8_t plen));
  void notify(const char* text);            // 状态通知
  String handleCommand(const String& cmd);  // 处理控制命令, 返回回复文本
  void update();                            // 周期调用(处理挂起通知)
  String handleUpload(const String& line);  // 上传协议处理(公开给回调类)
  String handleUploadSync(const String& line); // 串口路径: 同步直接写文件
  bool uploading() const { return _uploading; }

private:
  Storage* _st = nullptr;
  Player*  _player = nullptr;
  RmtOok*  _rf = nullptr;
  void (*_forward)(uint8_t, const uint8_t*, uint8_t) = nullptr;

  NimBLEServer* _server = nullptr;
  NimBLECharacteristic* _statusChr = nullptr;
  String _uploadName;
  String _uploadBuf;
  bool _uploading = false;
  bool _savePending = false;
};
