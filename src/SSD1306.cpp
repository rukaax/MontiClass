/** @file SSD1306.cpp - OLED 硬件初始化与各页面的具体绘制实现。 */
#include "SSD1306.h"  // 引入本模块公开的绘制函数声明。

#include <qrcode.h>  // 引入 ESP32 官方二维码生成库，用于配网页面。

#include "buzzer.h"  // OLED 总线无法探测时给出两声短提示。
#include "system_log.h"  // 把启动诊断同步提供给 WebUI。

namespace {  // 下面这些对象和函数只允许本源文件内部使用（相当于 C 的 static）。
// U8g2 的全缓冲对象。128x64 单色屏需要约 1 KB RAM，ESP32-S3 足够使用。
u8g2_t oled;  // 整块屏幕的显示缓冲区；所有绘制先写到它，再一次性刷屏。
bool oledReady = false;  // 只有目标地址应答后才允许 U8g2 访问 I2C 总线。

const char* i2cErrorName(uint8_t error) {  // 把 Wire.endTransmission() 的错误码转成可读文本。
  switch (error) {
    case 0: return "ACK";                         // 地址和数据都发送成功。
    case 1: return "data-too-long";               // 发送缓冲区溢出。
    case 2: return "address-NACK";                // 从设备没有应答地址。
    case 3: return "data-NACK";                   // 从设备没有应答数据。
    case 4: return "other-error";                 // 其他总线错误。
    case 5: return "timeout";                     // ESP32 Wire 超时（部分核心版本使用该码）。
    default: return "unknown-error";
  }
}

bool logOledI2cDiagnostics() {  // 启动时扫描 OLED 专用总线，便于区分接线、地址和驱动问题。
  SystemLog::printf("[OLED][I2C] bus SDA=%u SCL=%u clock=%luHz expected-7bit=0x%02X",
                static_cast<unsigned>(Pins::kOledSda),
                static_cast<unsigned>(Pins::kOledScl),
                static_cast<unsigned long>(Wire.getClock()),
                static_cast<unsigned>(kOledAddress));
  SystemLog::printf("[OLED][I2C] idle levels SDA=%s SCL=%s (HIGH normally means pull-up is present)",
                digitalRead(Pins::kOledSda) ? "HIGH" : "LOW",
                digitalRead(Pins::kOledScl) ? "HIGH" : "LOW");

  // I2C 空闲时两根线都必须由上拉电阻保持高电平；任一根被拉低时继续逐地址扫描
  // 可能在驱动层等待总线恢复，导致整个启动流程看起来“卡死”。
  const bool sdaIdle = digitalRead(Pins::kOledSda) == HIGH;
  const bool sclIdle = digitalRead(Pins::kOledScl) == HIGH;
  if (!sdaIdle || !sclIdle) {
    SystemLog::printf("[OLED][I2C] scan aborted: bus not idle (SDA=%s SCL=%s); check 3.3V/GND, SDA/SCL order, pull-ups and GPIO numbers",
                  sdaIdle ? "HIGH" : "LOW", sclIdle ? "HIGH" : "LOW");
    return false;
  }

  const uint16_t originalTimeout = Wire.getTimeOut();
  Wire.setTimeOut(20);  // 卡线时每个地址最多等待 20ms，避免 127 个地址拖慢启动。
  uint8_t deviceCount = 0;
  uint8_t targetError = 0xFF;
  for (uint8_t address = 0x01; address < 0x7F; ++address) {
    Wire.beginTransmission(address);  // 只探测地址，不发送寄存器数据。
    const uint8_t error = Wire.endTransmission();
    if (address == kOledAddress) targetError = error;
    if (error == 0) {
      ++deviceCount;
      SystemLog::printf("[OLED][I2C] ACK address=0x%02X", static_cast<unsigned>(address));
    }
  }
  Wire.setTimeOut(originalTimeout);  // 扫描后恢复 Wire/U8g2 原本的事务超时。

  if (targetError == 0) {
    SystemLog::printf("[OLED][I2C] target 0x%02X responded; SSD1306 init will continue",
                  static_cast<unsigned>(kOledAddress));
  } else {
    SystemLog::printf("[OLED][I2C] target 0x%02X probe failed: code=%u (%s)",
                  static_cast<unsigned>(kOledAddress),
                  static_cast<unsigned>(targetError), i2cErrorName(targetError));
  }
  SystemLog::printf("[OLED][I2C] scan complete: %u device(s)",
                static_cast<unsigned>(deviceCount));
  return targetError == 0;
}

void drawAlertMark(uint8_t y, bool alert) {  // 在某一行右侧画告警圆点；y 为该行基线。
  if (!alert) return;  // 没有告警就直接返回，不画任何东西。
  // 固定画在右侧，不能覆盖最多约 100 像素宽的中文/数值文本。
  u8g2_DrawDisc(&oled, 122, y - 5, 3, U8G2_DRAW_ALL);  // 在坐标(122, y-5)画半径3的实心圆点。
}

void drawQrCode(esp_qrcode_handle_t qrCode) {  // 二维码库的回调：把生成好的二维码画到屏上。
  const int size = esp_qrcode_get_size(qrCode);  // 取二维码边长（模块数，如 25 表示 25x25）。
  // 配网页面的 URL 很短，通常是 25x25 模块；以 2 像素绘制后仍可
  // 在左侧区域留下足够的白边，普通手机相机可稳定识别。
  constexpr int kModulePixels = 2;  // 每个二维码模块用 2x2 像素绘制（constexpr = 编译期常量）。
  // 二维码四周保留至少 4 个模块的白色静区，扫描器更容易定位图案。
  constexpr int kLeft = 8;  // 二维码左上角的 X 起点（像素）。
  constexpr int kTop = 8;   // 二维码左上角的 Y 起点（像素）。
  for (int y = 0; y < size; ++y) {  // 逐行遍历二维码的每个模块。
    for (int x = 0; x < size; ++x) {  // 逐列遍历二维码的每个模块。
      if (esp_qrcode_get_module(qrCode, x, y)) {  // 该模块为“黑点”时才需要绘制。
        u8g2_DrawBox(&oled, kLeft + x * kModulePixels, kTop + y * kModulePixels,  // 计算像素坐标并画一个实心方块。
                     kModulePixels, kModulePixels);  // 方块宽高都是 2 像素。
      }
    }
  }
}
}  // namespace  // 匿名命名空间结束。

void oledInit() {  // 初始化 OLED：配置 I2C、设置地址、点亮屏幕。
  // 先由 Wire 绑定自定义 SDA/SCL，再交给 U8g2 使用硬件 I2C 后端。
  const bool busReady = Wire.begin(Pins::kOledSda, Pins::kOledScl, 100000);  // 指定 OLED 使用的 SDA/SCL 引脚并启动 100kHz I2C。
  SystemLog::printf("[OLED][I2C] Wire.begin=%s", busReady ? "OK" : "FAILED");
  const bool i2cAvailable = busReady && logOledI2cDiagnostics();  // 在 U8g2 初始化前确认总线上是否能看到 OLED 地址。
  u8g2_Setup_ssd1306_i2c_128x64_noname_f(  // 按 128x64 SSD1306 全缓冲模式配置 U8g2 对象。
      &oled, U8G2_R0, u8x8_byte_arduino_hw_i2c, u8x8_gpio_and_delay_arduino);  // R0=不旋转，使用 Arduino 硬件 I2C。
  if (!i2cAvailable) {
    SystemLog::write("[OLED] disabled: I2C target unavailable; firmware will continue without OLED");
    buzzerAlarmTwoBeep();
    return;
  }
  // U8g2 接收 8 位 I2C 地址，所以常见的 7 位 0x3C 必须左移一位。
  u8g2_SetI2CAddress(&oled, kOledAddress << 1);  // 把 7 位地址左移 1 位变成 8 位地址传给库。
  u8g2_InitDisplay(&oled);  // 向屏幕发送初始化命令序列。
  u8g2_SetPowerSave(&oled, 0);  // 关闭省电模式，让屏幕正常显示。
  oledReady = true;
}

String oledI2cDevicesJson() {
  String result = "[";
  const uint16_t originalTimeout = Wire.getTimeOut();
  Wire.setTimeOut(20);
  bool first = true;
  for (uint8_t address = 0x08; address < 0x78; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() != 0) continue;
    if (!first) result += ',';
    char text[40];
    const char* module = address == kOledAddress ? "SSD1306 OLED" : "未知模块";
    snprintf(text, sizeof(text), "\"0x%02X (%s)\"", address, module);
    result += text;
    first = false;
  }
  Wire.setTimeOut(originalTimeout);
  result += ']';
  return result;
}

void drawBootScreen() {  // 绘制开机启动画面。
  if (!oledReady) return;
  // 所有画面均遵循 ClearBuffer -> Draw -> SendBuffer，避免残影。
  u8g2_ClearBuffer(&oled);  // 先清空缓冲区。
  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);  // 选用 12 像素高的中文字体。
  u8g2_DrawUTF8(&oled, 15, 24, "教室环境监测");  // 在(15,24)绘制中文标题。
  u8g2_SetFont(&oled, u8g2_font_5x7_tf);  // 切换到 5x7 小号英文字体。
  u8g2_DrawStr(&oled, 12, 43, "OLED + EC11 + Sensors");  // 绘制硬件构成说明。
  u8g2_DrawStr(&oled, 12, 55, "MQ135 warming up...");  // 提示 MQ135 正在预热。
  u8g2_SendBuffer(&oled);  // 把缓冲区一次性刷到屏幕。
}

void drawProvisioningScreen(const char* ssid, const char* qrPayload) {  // 绘制配网页面（含二维码）。
  if (!oledReady) return;
  u8g2_ClearBuffer(&oled);  // 清空缓冲区。
  // 二维码回调直接写入同一个帧缓冲；生成失败时仍会显示文字配网说明。
  esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();  // 取二维码默认配置作为基础。
  config.display_func = drawQrCode;  // 指定由本文件的 drawQrCode 负责实际绘制。
  config.max_qrcode_version = 2;  // 限制二维码版本，保证足够小、能放进屏幕左侧。
  config.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;  // 用最低纠错等级，让二维码更小更好扫。
  esp_qrcode_generate(&config, qrPayload);  // 根据要编码的字符串生成并（通过回调）绘制二维码。

  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);  // 中文字体。
  u8g2_DrawUTF8(&oled, 62, 17, "WiFi配网");  // 右侧标题。
  u8g2_SetFont(&oled, u8g2_font_5x7_tf);  // 小号英文字体。
  u8g2_DrawStr(&oled, 62, 31, "SSID:");  // 显示“SSID:”标签。
  u8g2_DrawStr(&oled, 62, 40, ssid);  // 显示配网热点名称。
  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);  // 切回中文字体。
  u8g2_DrawUTF8(&oled, 62, 52, "无密码");  // 提示热点无密码。
  u8g2_DrawUTF8(&oled, 62, 63, "热点持续开放");  // 提示热点常开。
  u8g2_SendBuffer(&oled);  // 刷屏。
}

void drawNetworkConnectingScreen(const char* ssid) {  // 绘制“正在连接 WiFi”提示页。
  if (!oledReady) return;
  u8g2_ClearBuffer(&oled);  // 清空缓冲区。
  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);  // 中文字体。
  u8g2_DrawUTF8(&oled, 8, 25, "正在连接 WiFi");  // 显示连接中提示。
  u8g2_SetFont(&oled, u8g2_font_5x7_tf);  // 小号英文字体。
  u8g2_DrawStr(&oled, 8, 42, ssid);  // 显示目标 WiFi 名称。
  u8g2_DrawStr(&oled, 8, 56, "Please wait...");  // 提示请稍候。
  u8g2_SendBuffer(&oled);  // 刷屏。
}

void drawMonitor(const SensorData& data, const EnvironmentStatus& status) {  // 绘制实时监测主页面。
  if (!oledReady) return;
  char line[32];  // 临时字符串缓冲，用来拼接每行文字。
  // 屏幕高度只有 64 像素：14 像素标题 + 4 行 12 像素正好容纳所有数据。
  u8g2_ClearBuffer(&oled);  // 清空缓冲区。
  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);  // 中文字体。

  u8g2_DrawBox(&oled, 0, 0, 128, 14);  // 顶部画一条 14 像素高的实心横条作标题底。
  u8g2_SetDrawColor(&oled, 0);  // 切成“0号色”（黑），以便在白条上写反色文字。
  // 标题按需求固定为“环境质量看板”；操作提示让位给更清晰的主标题。
  u8g2_DrawUTF8(&oled, 2, 12, "环境质量看板");  // 在标题条上写标题。
  u8g2_SetDrawColor(&oled, 1);  // 恢复“1号色”（白/亮）继续正常绘制。

  snprintf(line, sizeof(line), "光1:%2.0f%% 光2:%2.0f%%",  // 拼接两路光照百分比文字。
           data.light1Percent, data.light2Percent);  // %2.0f 表示不带小数、至少 2 位宽。
  u8g2_DrawUTF8(&oled, 2, 26, line);  // 在第 1 行绘制光照数据。
  drawAlertMark(26, status.lightAlert);  // 若光照告警则在右侧画圆点。

  // 预热期间展示 raw ADC；基线建立后显示 0..500 相对 AQI。
  if (data.mq135WarmedUp) {  // MQ135 已预热完成时。
    snprintf(line, sizeof(line), "空气AQI: %3.0f", data.mq135AqiEstimate);  // 显示估算的 AQI。
  } else {  // 尚在预热时。
    snprintf(line, sizeof(line), "空气:预热 %4u", data.mq135Raw);  // 显示原始 ADC 数值。
  }
  u8g2_DrawUTF8(&oled, 2, 38, line);  // 在第 2 行绘制空气数据。
  drawAlertMark(38, status.airAlert);  // 若空气告警则画圆点。

  // DHT11 本轮读取失败时用占位符；不会画出上一轮缓存的数据。
  if (data.dhtValid) snprintf(line, sizeof(line), "温度: %4.1fC", data.temperatureC);  // 读数有效则显示温度。
  else snprintf(line, sizeof(line), "温度: --.-C");  // 读数无效则显示占位符。
  u8g2_DrawUTF8(&oled, 2, 50, line);  // 在第 3 行绘制温度。
  drawAlertMark(50, status.temperatureAlert);  // 若温度告警则画圆点。

  if (data.dhtValid) snprintf(line, sizeof(line), "湿度: %4.0f%%", data.humidityPercent);  // 有效则显示湿度。
  else snprintf(line, sizeof(line), "湿度: --%%");  // 无效则显示占位符（%% 打印为一个 %）。
  u8g2_DrawUTF8(&oled, 2, 62, line);  // 在第 4 行绘制湿度。
  drawAlertMark(62, status.humidityAlert);  // 若湿度告警则画圆点。

  u8g2_SendBuffer(&oled);  // 一次性刷屏。
}

void drawSettings(const Thresholds& thresholds, uint8_t selectedIndex, bool editing,  // 绘制设置页面。
                  const char* ipAddress, bool stationConnected,  // 传入 IP 与联网状态用于网络信息页。
                  const ReportUploadStatus& reportStatus, bool showNetworkInfo) {  // 传入上报状态与是否显示网络页。
  if (!oledReady) return;
  // 这里使用局部快照数组，仅用于把结构体按固定菜单顺序渲染；修改操作
  // 仍由 main.cpp 统一完成，避免显示层拥有业务状态。
  const char* labels[] = {"室外阈值", "室内目标", "空气", "温度", "湿度"};  // 各设置项的中文名。
  const float values[] = {thresholds.outdoorLightThresholdPercent,  // 各设置项当前值（顺序与 labels 对应）。
                          thresholds.lightHoldPercent, thresholds.mq135Aqi,  // 光照目标、空气 AQI。
                          thresholds.temperatureC, thresholds.humidityPercent};  // 温度、湿度。
  const char* units[] = {"%", "%", "AQI", "C", "%"};  // 各设置项的单位。
  char line[28];  // 拼接每行文字用的缓冲。

  u8g2_ClearBuffer(&oled);  // 清空缓冲区。
  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);  // 中文字体。
  u8g2_DrawBox(&oled, 0, 0, 128, 14);  // 顶部标题底条。
  u8g2_SetDrawColor(&oled, 0);  // 切黑色写反白标题。
  const char* title = showNetworkInfo ? "网络信息  向上返回" :  // 根据当前页选择标题：网络信息页。
                      (editing ? "调节设置  短按确认" : "选择项目  短按调节");  // 编辑中 / 仅选择时的不同提示。
  u8g2_DrawUTF8(&oled, 2, 12, title);  // 绘制标题。
  u8g2_SetDrawColor(&oled, 1);  // 恢复正常绘制色。

  if (showNetworkInfo) {  // 若当前是只读的网络信息页。
    char elapsed[16];  // 存放“多久之前”的短文本。
    const auto formatElapsed = [](bool available, uint32_t timestamp, char* output,  // 定义一个局部 lambda：把时间戳格式化成“XX前”。
                                  size_t outputSize) {  // lambda 就是 C++ 里写在函数内部的小函数。
      if (!available) {  // 没有记录时。
        snprintf(output, outputSize, "无记录");  // 显示“无记录”。
        return;  // 结束 lambda。
      }
      const uint32_t seconds = (millis() - timestamp) / 1000;  // 计算距离该时间戳过去了多少秒。
      if (seconds < 5) snprintf(output, outputSize, "刚刚");  // 5 秒内显示“刚刚”。
      else if (seconds < 60) snprintf(output, outputSize, "%lu秒前",  // 1 分钟内显示“X秒前”。
                                     static_cast<unsigned long>(seconds));  // static_cast 做类型转换以匹配 %lu。
      else if (seconds < 3600) snprintf(output, outputSize, "%lu分前",  // 1 小时内显示“X分前”。
                                       static_cast<unsigned long>(seconds / 60));  // 换算成分钟。
      else snprintf(output, outputSize, "%lu时前",  // 超过 1 小时显示“X时前”。
                    static_cast<unsigned long>(seconds / 3600));  // 换算成小时。
    };

    u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);  // 中文字体。
    snprintf(line, sizeof(line), "IP: %s", stationConnected ? ipAddress : "未连接");  // 已连接显示 IP，否则“未连接”。
    u8g2_DrawUTF8(&oled, 3, 26, line);  // 绘制 IP 行。

    formatElapsed(reportStatus.hasDataUpload, reportStatus.lastDataUploadMs,  // 计算距上次数据上报多久。
                  elapsed, sizeof(elapsed));  // 结果写入 elapsed。
    snprintf(line, sizeof(line), "数据上报: %s", elapsed);  // 拼接数据上报行。
    u8g2_DrawUTF8(&oled, 3, 38, line);  // 绘制数据上报行。

    formatElapsed(reportStatus.hasHeartbeatUpload, reportStatus.lastHeartbeatUploadMs,  // 计算距上次心跳多久。
                  elapsed, sizeof(elapsed));  // 结果写入 elapsed。
    snprintf(line, sizeof(line), "心跳上报: %s", elapsed);  // 拼接心跳上报行。
    u8g2_DrawUTF8(&oled, 3, 50, line);  // 绘制心跳上报行。

    const char* apiText = "待检测";  // API 连通状态默认文本。
    if (reportStatus.apiConnectivity == ApiConnectivity::NotConfigured) apiText = "未配置";  // 未配置服务器。
    else if (!stationConnected ||  // 未联网，或
             reportStatus.apiConnectivity == ApiConnectivity::Unreachable) apiText = "不通";  // 判定为不可达。
    else if (reportStatus.apiConnectivity == ApiConnectivity::Reachable) apiText = "连通";  // 已确认可达。
    snprintf(line, sizeof(line), "API: %s", apiText);  // 拼接 API 状态行。
    u8g2_DrawUTF8(&oled, 3, 62, line);  // 绘制 API 状态行。
    u8g2_SendBuffer(&oled);  // 刷屏。
    return;  // 网络信息页绘制完成，结束函数。
  }

  const uint8_t firstVisibleIndex = selectedIndex > 3 ? selectedIndex - 3 : 0;  // 计算滚动窗口起点，保证选中项可见。
  for (uint8_t row = 0; row < 4; ++row) {  // 一屏最多显示 4 行设置项。
    // 基线从 26 到 62，每行相隔 12 像素，与 wqy12 字体高度匹配。
    const uint8_t y = 26 + row * 12;  // 本行文字基线的 Y 坐标。
    const uint8_t thresholdIndex = firstVisibleIndex + row;  // 本行对应的设置项索引。
    if (thresholdIndex == 3) {  // 索引 3（温度）需要一位小数。
      snprintf(line, sizeof(line), "%s: %4.1f%s", labels[thresholdIndex],  // 用 %4.1f 拼接带小数的行。
               values[thresholdIndex], units[thresholdIndex]);  // 填入值和单位。
    } else {  // 其他项显示整数。
      snprintf(line, sizeof(line), "%s: %4.0f%s", labels[thresholdIndex],  // 用 %4.0f 拼接整数行。
               values[thresholdIndex], units[thresholdIndex]);  // 填入值和单位。
    }
    if (thresholdIndex == selectedIndex) {  // 当前行是被选中的项。
      if (editing) {  // 正在编辑该项时。
        // 实心反白代表“旋转会立即改数值”；边框仅代表当前光标位置。
        u8g2_DrawBox(&oled, 0, y - 10, 128, 12);  // 画整行实心背景。
        u8g2_SetDrawColor(&oled, 0);  // 切黑色写反白文字。
        u8g2_DrawUTF8(&oled, 3, y, line);  // 绘制反白文字。
        u8g2_SetDrawColor(&oled, 1);  // 恢复正常绘制色。
      } else {  // 选中但未进入编辑时。
        u8g2_DrawFrame(&oled, 0, y - 10, 128, 12);  // 只画一个空心边框表示光标位置。
        u8g2_DrawUTF8(&oled, 3, y, line);  // 正常绘制文字。
      }
    } else {  // 非选中行。
      u8g2_DrawUTF8(&oled, 3, y, line);  // 直接正常绘制文字。
    }
  }
  u8g2_SendBuffer(&oled);  // 一次性刷屏。
}
