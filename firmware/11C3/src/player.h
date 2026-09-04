#pragma once
#include <Arduino.h>
#include <FS.h>
#include "protocol.h"
#include "rmt_ook.h"
#include "storage.h"

// 独立播放引擎: 从 LittleFS 流式播放节目, 按时间戳经 433MHz 发帧
class Player {
public:
  enum State { IDLE, PLAYING, PAUSED };

  void begin(Storage* storage, RmtOok* rf);
  bool play(const String& name);     // 开始播放(可替换正在播的)
  void stop();
  void toggle();                     // 播放/暂停
  void next(const String* names, int count);  // 切到下一个节目并播放
  State state() const { return _state; }
  const String& current() const { return _current; }
  void tick();                       // 高频调用: 推进时间轴
  void sendPayload(const uint8_t* payload20);   // 发一帧(应用亮度+组帧)
  void setBrightness(uint8_t pct) { _brightness = pct; }
  uint8_t brightness() const { return _brightness; }
  void setResendMs(uint32_t ms) { _resendMs = ms; }
  // 当前帧重发(调色/亮度变化后立即生效)
  void resendCurrent();

private:
  void readNext();       // 读下一帧到 _next*
  void openCurrent();
  uint32_t elapsedMs();

  Storage* _st = nullptr;
  RmtOok*  _rf = nullptr;
  File _f;
  State _state = IDLE;
  String _current;
  uint32_t _startUs = 0, _pauseUs = 0;
  bool _hasNext = false;
  uint32_t _nextTime = 0;
  uint8_t _nextPayload[STREAM_PAYLOAD_LEN];
  uint8_t _lastPayload[STREAM_PAYLOAD_LEN];
  bool _hasLast = false;
  uint8_t _brightness = 100;
  uint32_t _resendMs = 1000;         // 播放中周期性重发当前帧, 0=关闭
  uint32_t _lastResend = 0;
  bool _longFired = false;
};
