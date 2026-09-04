#include "button.h"

void Button::begin(uint8_t pin, void (*onEvent)(BtnEvent)) {
  _pin = pin;
  _onEvent = onEvent;
  pinMode(_pin, INPUT_PULLUP);
}

void Button::update() {
  uint32_t now = millis();
  bool down = (digitalRead(_pin) == LOW);

  if (down && !_pressed) {
    _pressed = true;
    _pressStart = now;
    _longFired = false;
    _veryFired = false;
  } else if (!down && _pressed) {
    // 松开
    uint32_t held = now - _pressStart;
    _pressed = false;
    if (!_longFired) {
      // 单击候选: 若之前已有待确认单击且在双击窗口内 -> 双击
      if (_pendingShortAt && (now - _pendingShortAt) <= BTN_DOUBLE_GAP) {
        _pendingShortAt = 0;
        if (_onEvent) _onEvent(BTN_DOUBLE);
      } else {
        _pendingShortAt = now;    // 等待双击窗口
      }
    }
  }

  if (_pressed) {
    uint32_t held = now - _pressStart;
    if (!_longFired && held >= BTN_LONG_MS) {
      _longFired = true;
      if (_onEvent) _onEvent(BTN_LONG);
    }
    if (!_veryFired && held >= BTN_VERY_LONG_MS) {
      _veryFired = true;
      if (_onEvent) _onEvent(BTN_VERY_LONG);
    }
  }

  // 双击窗口超时 -> 判定为单击
  if (!_pressed && _pendingShortAt && (now - _pendingShortAt) > BTN_DOUBLE_GAP) {
    _pendingShortAt = 0;
    if (_onEvent) _onEvent(BTN_SHORT);
  }
}
