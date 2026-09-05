#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// LumaFlow TLV 帧格式 (与 LumaFlow core/serial_protocol.py 一致)
//   EB 90 | len(=1+plen) | cmd | payload | checksum | ED
//   checksum = (len + cmd + sum(payload)) & 0xFF
#define TLV_HEAD0           0xEB
#define TLV_HEAD1           0x90
#define TLV_TAIL            0xED
#define CMD_STREAM          0xD8
#define CMD_AUTH            0xE0
#define STREAM_PAYLOAD_LEN  20          // 10 通道 x 2 字节
#define TLV_STREAM_FRAME_LEN 26         // 2+1+1+20+1+1
#define TLV_MAX_LEN         128

// 单通道解出的值
struct ChVal { uint8_t func; uint8_t r, g, b; };

// 组帧: 返回帧长度(<=TLV_MAX_LEN), 失败返回 0
size_t build_tlv(uint8_t cmd, const uint8_t* payload, uint8_t plen, uint8_t* out);
uint8_t tlv_checksum(uint8_t len, uint8_t cmd, const uint8_t* payload, uint8_t plen);
// 判断缓冲区是否是一帧 TLV (至少 6 字节, 头尾正确)
bool looks_like_tlv(const uint8_t* data, size_t len);
// 解析 20 字节 STREAM payload -> 10 通道
void decode_stream_payload(const uint8_t* p, ChVal out[10]);

// 流式 TLV 解析器: 喂字节, 完整帧触发回调 (处理分片/粘包)
typedef void (*tlv_frame_cb)(uint8_t cmd, const uint8_t* payload, uint8_t plen);

class TlvParser {
public:
  TlvParser() : _cb(nullptr) { reset(); }
  void reset();
  void setCallback(tlv_frame_cb cb) { _cb = cb; }
  // 返回 true 表示本字节完成了一帧并已回调
  bool feed(uint8_t b);
  size_t buffered() const { return _idx; }
private:
  enum State { S_HEAD0, S_HEAD1, S_LEN, S_CMD, S_PAYLOAD, S_CK, S_TAIL };
  State  _st = S_HEAD0;
  uint8_t _buf[TLV_MAX_LEN];
  uint8_t _len = 0, _idx = 0, _cmd = 0;
  tlv_frame_cb _cb = nullptr;
};
