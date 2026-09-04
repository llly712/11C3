#include "player.h"
#include <LittleFS.h>

void Player::begin(Storage* storage, RmtOok* rf) {
  _st = storage;
  _rf = rf;
  _brightness = _st ? _st->getBrightness() : 100;
}

uint32_t Player::elapsedMs() {
  if (_state != PLAYING) return 0;
  return (uint32_t)((micros() - _startUs) / 1000ULL);
}

void Player::openCurrent() {
  _f = LittleFS.open("/seq/" + _current + ".bin", "r");
  _hasNext = false;
  if (!_f) return;
  uint8_t magic[4];
  uint16_t count = 0;
  if (_f.read(magic, 4) != 4 || memcmp(magic, "LFC3", 4) != 0) { _f.close(); return; }
  _f.read((uint8_t*)&count, 2);
  if (count == 0) { _f.close(); return; }
  readNext();
}

void Player::readNext() {
  if (!_f) return;
  uint8_t hdr[4];
  if (_f.read(hdr, 4) != 4) { _hasNext = false; return; }  // 文件读完
  memcpy(&_nextTime, hdr, 4);
  if (_f.read(_nextPayload, STREAM_PAYLOAD_LEN) != STREAM_PAYLOAD_LEN) { _hasNext = false; return; }
  _hasNext = true;
}

bool Player::play(const String& name) {
  String safe = _st ? _st->sanitizeName(name) : name;
  File probe = LittleFS.open("/seq/" + safe + ".bin", "r");
  if (!probe) return false;
  probe.close();

  _current = safe;
  _state = IDLE;
  openCurrent();
  if (!_f) return false;

  _state = PLAYING;
  _startUs = micros();
  _pauseUs = 0;
  _longFired = false;
  _lastResend = 0;
  if (_st) _st->setCurrentPreset(safe);
  if (_hasNext && _nextTime == 0) {
    sendPayload(_nextPayload);
    readNext();
  }
  return true;
}

void Player::stop() {
  _state = IDLE;
  if (_f) _f.close();
  _hasNext = false;
}

void Player::toggle() {
  if (_state == PLAYING) {
    _state = PAUSED;
    _pauseUs = micros();
  } else if (_state == PAUSED) {
    _state = PLAYING;
    _startUs += micros() - _pauseUs;   // 恢复时间轴
    _pauseUs = 0;
    _lastResend = 0;
  } else {
    // IDLE: 播放当前(或默认)节目
    String cur = _current;
    if (cur.length() == 0 && _st) cur = _st->getCurrentPreset();
    if (cur.length() > 0) play(cur);
  }
}

void Player::next(const String* names, int count) {
  if (count <= 0) return;
  int idx = 0;
  for (int i = 0; i < count; i++) if (names[i] == _current) { idx = (i + 1) % count; break; }
  play(names[idx]);
}

void Player::tick() {
  if (_state != PLAYING || !_hasNext) {
    if (_state == PLAYING && !_hasNext) stop();
    return;
  }
  uint32_t now = elapsedMs();
  // 循环播放
  if (!_f && !_hasNext) {
    if (_st && _st->getLoopPlay()) { openCurrent(); if (_hasNext) _startUs = micros(); }
    else stop();
    return;
  }
  while (_hasNext && _nextTime <= now) {
    sendPayload(_nextPayload);
    readNext();
    if (!_hasNext) {
      if (_st && _st->getLoopPlay()) {
        // 重新打开文件从头播
        _startUs = micros();
        openCurrent();
      } else {
        stop();
      }
      return;
    }
  }
  // 周期性重发当前帧
  if (_resendMs > 0 && _hasLast) {
    uint32_t t = millis();
    if (t - _lastResend >= _resendMs) {
      _lastResend = t;
      sendPayload(_lastPayload);
    }
  }
}

void Player::sendPayload(const uint8_t* payload20) {
  if (!payload20 || !_rf) return;
  uint8_t scaled[STREAM_PAYLOAD_LEN];
  if (_brightness >= 100) {
    memcpy(scaled, payload20, STREAM_PAYLOAD_LEN);
  } else {
    for (int i = 0; i < 10; i++) {
      uint8_t a = payload20[i * 2], b = payload20[i * 2 + 1];
      uint8_t func = (a >> 4) & 0x0F;
      uint8_t r = (uint8_t)((int)(a & 0x0F) * _brightness + 50) / 100;
      uint8_t g = (uint8_t)((int)((b >> 4) & 0x0F) * _brightness + 50) / 100;
      uint8_t bl = (uint8_t)((int)(b & 0x0F) * _brightness + 50) / 100;
      scaled[i * 2]     = (uint8_t)((func << 4) | (r & 0x0F));
      scaled[i * 2 + 1] = (uint8_t)(((g & 0x0F) << 4) | (bl & 0x0F));
    }
  }
  _rf->sendFieldD8(scaled);
  memcpy(_lastPayload, scaled, STREAM_PAYLOAD_LEN);
  _hasLast = true;
}

void Player::resendCurrent() {
  if (_hasLast) {
    _rf->sendFieldD8(_lastPayload);
  }
}
