#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include "driver/rmt_tx.h"

// 用 RMT 在任意 GPIO 上输出 OOK/ASK 位流 (驱动 F113 等 433MHz ASK 发射模块)
// 两种编码:
//   MODE_NRZ : 纯 NRZ, LSB 优先, 空闲电平=低 (无载波)
//   MODE_UART: 模拟 UART 8N1, 空闲=高, 每字节 起始位0+8数据位+停止位1
class RmtOok {
public:
  enum Mode { MODE_NRZ = 0, MODE_UART = 1 };

  bool begin(gpio_num_t pin);
  void setBaud(uint32_t baud);            // 默认 9600
  void setMode(Mode m);
  void setInvert(bool invert);            // 电平反转
  void setPreamble(uint8_t bytes);        // 前导 0xFF 字节数 (连续载波)
  bool send(const uint8_t* data, size_t len);   // 带前导 + 数据, 同步
  bool sendEv1527(uint32_t code24, uint8_t repeat, uint32_t tUs);  // EV1527 编码发射
  // 自定义 OOK 场控帧: 固定同步前缀 + 每数据位三倍展开
  bool sendFieldFrame(const uint8_t* data, size_t len, uint8_t repeat = 6,
                      uint32_t bitUs = 250);
  // 同上, 但电平反相(诊断 F113 DATA 极性)
  bool sendFieldFrameInv(const uint8_t* data, size_t len, uint8_t repeat = 6,
                         uint32_t bitUs = 250);
  // 把 20 字节 STREAM payload(10通道×2B) 转成 21 字节场控帧, 按 phase 序列连发
  bool sendFieldD8(const uint8_t payload20[20]);
  bool carrier(uint32_t durationMs);    // 诊断: 连续载波(纯高电平), 验证发射模块
  void release();                        // 释放 RMT 通道, GPIO 变回普通输出(诊断用)
  bool busy() const;

private:
  rmt_channel_handle_t _chan = nullptr;
  rmt_encoder_handle_t _enc  = nullptr;
  uint32_t _baud = 9600;
  Mode _mode = MODE_NRZ;
  bool _invert = false;
  uint8_t _preamble = 0;
  bool _busy = false;
};
