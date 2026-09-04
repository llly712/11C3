#pragma once
// ================= 引脚定义 =================
#define PIN_ASK_TX     3   // F113 433MHz ASK 发射模块 DATA 引脚
#define PIN_BUTTON     5   // 按钮, 另一端接 GND (内部上拉, 按下为低)
// 无板载 WS2812 状态灯, 已去掉 PIN_STATUS_LED (固件内有 #ifdef 保护)

// ================= RF 默认参数 (可在 NVS / 网页 / BLE 修改) =================
#define RF_BAUD_DEFAULT       9600   // OOK 波特率
#define RF_PREAMBLE_DEFAULT   0      // 前导 0xFF 字节数
#define RF_MODE_DEFAULT       0      // 0=NRZ (纯位流), 1=UART (8N1 起止位)
#define RF_INVERT_DEFAULT     false  // 电平反转 (某些 ASK 模块反相)

// ================= 按钮逻辑 (毫秒) =================
#define BTN_DEBOUNCE_MS    25
#define BTN_DOUBLE_GAP     350    // 两次短按间隔
#define BTN_LONG_MS        1000   // 长按 1s
#define BTN_VERY_LONG_MS   5000   // 超长按 5s = 恢复出厂

// ================= BLE =================
#define BLE_NAME_PREFIX  "11C3"        // 手机 App / PC 靠这个前缀发现设备
// 11C3 控制服务 (协议与 LumaFlow 兼容)
#define BLE_SVC_LUMA     "7e570001-3e74-4b1e-b6f4-1a91f7080001"
#define BLE_CHR_LUMA     "7e570002-3e74-4b1e-b6f4-1a91f7080002"
// 手机上传/控制服务 (自定义)
#define BLE_SVC_CTRL     "c3a50001-3e74-4b1e-b6f4-1a91f7080001"
#define BLE_CHR_CTRL     "c3a50002-3e74-4b1e-b6f4-1a91f7080002"  // 命令写
#define BLE_CHR_UPLOAD   "c3a50003-3e74-4b1e-b6f4-1a91f7080003"  // 数据写(CSV)
#define BLE_CHR_STATUS   "c3a50004-3e74-4b1e-b6f4-1a91f7080004"  // 状态通知

// ================= WiFi =================
#define WIFI_AP_SSID_DEFAULT  "11C3"
#define WIFI_AP_PASS_DEFAULT  "12345678"
#define UDP_PORT              32712   // 11C3 UDP 固定端口

// ================= 容量限制 =================
#define MAX_PRESETS       16      // 最多保存的节目数
#define MAX_CSV_UPLOAD    100000  // 单帧最大字节(流式上传不再整体缓冲)
#define MAX_CSV_FRAMES    16384   // 单个节目最多帧数(解析上限; 一首歌 ~1 万帧)
#define MAX_UPLOAD_NAME   32

// ================= 调试 =================
// 定义后 Serial 输出日志(文本行, 不干扰二进制 TLV 协议)
#define FIRMWARE_LOG
