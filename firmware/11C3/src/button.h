#pragma once
#include "config.h"
#include <Arduino.h>

// 单按钮状态机: 短按 / 双击 / 长按(1s) / 超长按(5s)
// 按钮接 GND, 使用内部上拉, 按下为 LOW
enum BtnEvent {
  BTN_NONE = 0,
  BTN_SHORT,       // 短按(单击)
  BTN_DOUBLE,      // 双击
  BTN_LONG,        // 长按 1s(按住时触发一次)
  BTN_VERY_LONG    // 超长按 5s(按住时触发一次)
};

class Button {
public:
  void begin(uint8_t pin, void (*onEvent)(BtnEvent));
  void update();   // 高频调用(建议每 5-10ms)
private:
  uint8_t _pin = 0;
  void (*_onEvent)(BtnEvent) = nullptr;
  bool _pressed = false;
  uint32_t _pressStart = 0;
  bool _longFired = false, _veryFired = false;
  uint32_t _pendingShortAt = 0;   // 待确认的单击时间
};
