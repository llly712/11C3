#pragma once
#include "config.h"
#include "protocol.h"
#include <Arduino.h>
#include <Preferences.h>
#include <FS.h>

// 节目二进制格式 (LittleFS 文件 /seq/NAME.bin):
//   'L' 'F' 'C' '3' | u16 count | 重复 { u32 time_ms | 20B payload }
struct SeqEntry { uint32_t time_ms; uint8_t payload[STREAM_PAYLOAD_LEN]; };

class Storage {
public:
  bool begin();

  // ---- NVS 配置 ----
  uint32_t getRfBaud();      void setRfBaud(uint32_t v);
  uint8_t  getRfPreamble();  void setRfPreamble(uint8_t v);
  uint8_t  getRfMode();      void setRfMode(uint8_t v);
  bool     getRfInvert();    void setRfInvert(bool v);
  uint8_t  getBrightness();  void setBrightness(uint8_t pct);   // 0-100
  bool     getLoopPlay();    void setLoopPlay(bool v);
  String   getCurrentPreset(); void setCurrentPreset(const String& name);
  String   getApPass();      void setApPass(const String& v);
  String   getStaSsid();     void setStaSsid(const String& v);
  String   getStaPass();     void setStaPass(const String& v);

  // ---- 节目管理 ----
  String sanitizeName(const String& name);
  bool   presetExists(const String& name);
  // 解析 CSV 文本 -> 保存为二进制节目; 返回错误信息("" 表示成功)
  String saveCsvAsPreset(const String& name, const String& csvText);

  // 流式 CSV 写入: 避免把整个 CSV 缓冲进 RAM, 逐块喂入
  bool    beginCsvWrite(const String& name);   // 创建文件+写头部, 返回是否就绪
  bool    appendCsvData(const String& chunk);  // 追加一块 CSV 文本 (自动按行解析)
  String  endCsvWrite();                       // 写帧数+关闭, 返回错误(""成功)

  bool   loadPreset(const String& name, File& out);   // 打开节目文件
  int    listPresets(String* names, int max);         // 返回数量
  bool   deletePreset(const String& name);
  void   eraseAll();                                  // 清空全部 + 恢复默认配置
  String fsInfo();                                    // 调试: 列出 /seq 文件及大小

  // 从 CSV 文本生成一帧 payload (亮度由调用方决定是否缩放)
  static bool csvLineToPayload(const String& line, const String& header, uint8_t* payload20);

private:
  Preferences _pref;
  bool _fsReady = false;

  // 流式 CSV 写入状态
  File     _csvFile;         // 正在写入的节目文件
  String   _csvHeader;       // 解析到的表头
  String   _csvLineBuf;      // 跨块残留的半行
  uint16_t _csvCount = 0;    // 已写入帧数
  bool     _csvWriteActive = false;
};
