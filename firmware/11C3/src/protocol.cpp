#include "protocol.h"

uint8_t tlv_checksum(uint8_t len, uint8_t cmd, const uint8_t* payload, uint8_t plen) {
  uint32_t sum = (uint32_t)len + cmd;
  for (uint8_t i = 0; i < plen; i++) sum += payload[i];
  return (uint8_t)(sum & 0xFF);
}

size_t build_tlv(uint8_t cmd, const uint8_t* payload, uint8_t plen, uint8_t* out) {
  if (plen > 127) return 0;
  uint8_t len = 1 + plen;
  out[0] = TLV_HEAD0;
  out[1] = TLV_HEAD1;
  out[2] = len;
  out[3] = cmd;
  if (plen) memcpy(out + 4, payload, plen);
  out[4 + plen] = tlv_checksum(len, cmd, payload, plen);
  out[5 + plen] = TLV_TAIL;
  return 6 + plen;
}

bool looks_like_tlv(const uint8_t* data, size_t len) {
  return len >= 6 && data[0] == TLV_HEAD0 && data[1] == TLV_HEAD1 && data[len - 1] == TLV_TAIL;
}

void decode_stream_payload(const uint8_t* p, ChVal out[10]) {
  for (int i = 0; i < 10; i++) {
    uint8_t a = p[i * 2], b = p[i * 2 + 1];
    out[i].func = (a >> 4) & 0x0F;
    out[i].r    = a & 0x0F;
    out[i].g    = (b >> 4) & 0x0F;
    out[i].b    = b & 0x0F;
  }
}

void TlvParser::reset() {
  _st = S_HEAD0; _len = 0; _idx = 0; _cmd = 0;
}

bool TlvParser::feed(uint8_t b) {
  bool completed = false;
  switch (_st) {
    case S_HEAD0:
      if (b == TLV_HEAD0) _st = S_HEAD1;            // 否则丢弃
      break;
    case S_HEAD1:
      _st = (b == TLV_HEAD1) ? S_LEN : S_HEAD0;
      break;
    case S_LEN:
      _len = b;
      _idx = 0;
      _st  = (_len >= 2 && _len <= TLV_MAX_LEN - 4) ? S_CMD : S_HEAD0;
      break;
    case S_CMD:
      _cmd = b;
      _buf[_idx++] = b;
      _st = (_len - 1 > 0) ? S_PAYLOAD : S_CK;
      break;
    case S_PAYLOAD:
      _buf[_idx++] = b;
      if (_idx == _len) _st = S_CK;                 // payload 收完
      break;
    case S_CK: {
      uint8_t ck = tlv_checksum(_len, _cmd, _buf + 1, _len - 1);
      _st = (ck == b) ? S_TAIL : S_HEAD0;
      break;
    }
    case S_TAIL:
      if (b == TLV_TAIL && _cb) {
        // _buf[0]=cmd, _buf[1..len-1]=payload
        _cb(_buf[0], _buf + 1, _len - 1);
        completed = true;
      }
      reset();
      break;
  }
  return completed;
}
