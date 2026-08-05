/** @file SSD1306.cpp */
#include "SSD1306.h"

#include <qrcode.h>

namespace {
// U8g2 的全缓冲对象。128x64 单色屏需要约 1 KB RAM，ESP32-S3 足够使用。
u8g2_t oled;

void drawAlertMark(uint8_t y, bool alert) {
  if (!alert) return;
  // 固定画在右侧，不能覆盖最多约 100 像素宽的中文/数值文本。
  u8g2_DrawDisc(&oled, 122, y - 5, 3, U8G2_DRAW_ALL);
}

void drawQrCode(esp_qrcode_handle_t qrCode) {
  const int size = esp_qrcode_get_size(qrCode);
  // 配网页面的 URL 很短，通常是 25x25 模块；以 2 像素绘制后仍可
  // 在左侧区域留下足够的白边，普通手机相机可稳定识别。
  constexpr int kModulePixels = 2;
  // 二维码四周保留至少 4 个模块的白色静区，扫描器更容易定位图案。
  constexpr int kLeft = 8;
  constexpr int kTop = 8;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      if (esp_qrcode_get_module(qrCode, x, y)) {
        u8g2_DrawBox(&oled, kLeft + x * kModulePixels, kTop + y * kModulePixels,
                     kModulePixels, kModulePixels);
      }
    }
  }
}
}  // namespace

void oledInit() {
  // 先由 Wire 绑定自定义 SDA/SCL，再交给 U8g2 使用硬件 I2C 后端。
  Wire.begin(Pins::kOledSda, Pins::kOledScl);
  u8g2_Setup_ssd1306_i2c_128x64_noname_f(
      &oled, U8G2_R0, u8x8_byte_arduino_hw_i2c, u8x8_gpio_and_delay_arduino);
  // U8g2 接收 8 位 I2C 地址，所以常见的 7 位 0x3C 必须左移一位。
  u8g2_SetI2CAddress(&oled, kOledAddress << 1);
  u8g2_InitDisplay(&oled);
  u8g2_SetPowerSave(&oled, 0);
}

void drawBootScreen() {
  // 所有画面均遵循 ClearBuffer -> Draw -> SendBuffer，避免残影。
  u8g2_ClearBuffer(&oled);
  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);
  u8g2_DrawUTF8(&oled, 15, 24, "教室环境监测");
  u8g2_SetFont(&oled, u8g2_font_5x7_tf);
  u8g2_DrawStr(&oled, 12, 43, "OLED + EC11 + Sensors");
  u8g2_DrawStr(&oled, 12, 55, "MQ135 warming up...");
  u8g2_SendBuffer(&oled);
}

void drawProvisioningScreen(const char* ssid, const char* qrPayload) {
  u8g2_ClearBuffer(&oled);
  // 二维码回调直接写入同一个帧缓冲；生成失败时仍会显示文字配网说明。
  esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();
  config.display_func = drawQrCode;
  config.max_qrcode_version = 2;
  config.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  esp_qrcode_generate(&config, qrPayload);

  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);
  u8g2_DrawUTF8(&oled, 62, 17, "WiFi配网");
  u8g2_SetFont(&oled, u8g2_font_5x7_tf);
  u8g2_DrawStr(&oled, 62, 31, "SSID:");
  u8g2_DrawStr(&oled, 62, 40, ssid);
  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);
  u8g2_DrawUTF8(&oled, 62, 52, "无密码");
  u8g2_DrawUTF8(&oled, 62, 63, "热点持续开放");
  u8g2_SendBuffer(&oled);
}

void drawNetworkConnectingScreen(const char* ssid) {
  u8g2_ClearBuffer(&oled);
  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);
  u8g2_DrawUTF8(&oled, 8, 25, "正在连接 WiFi");
  u8g2_SetFont(&oled, u8g2_font_5x7_tf);
  u8g2_DrawStr(&oled, 8, 42, ssid);
  u8g2_DrawStr(&oled, 8, 56, "Please wait...");
  u8g2_SendBuffer(&oled);
}

void drawMonitor(const SensorData& data, const EnvironmentStatus& status) {
  char line[32];
  // 屏幕高度只有 64 像素：14 像素标题 + 4 行 12 像素正好容纳所有数据。
  u8g2_ClearBuffer(&oled);
  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);

  u8g2_DrawBox(&oled, 0, 0, 128, 14);
  u8g2_SetDrawColor(&oled, 0);
  // 标题按需求固定为“环境质量看板”；操作提示让位给更清晰的主标题。
  u8g2_DrawUTF8(&oled, 2, 12, "环境质量看板");
  u8g2_SetDrawColor(&oled, 1);

  snprintf(line, sizeof(line), "光1:%2.0f%% 光2:%2.0f%%",
           data.light1Percent, data.light2Percent);
  u8g2_DrawUTF8(&oled, 2, 26, line);
  drawAlertMark(26, status.lightAlert);

  // 预热期间展示 raw ADC；基线建立后显示 0..500 相对 AQI。
  if (data.mq135WarmedUp) {
    snprintf(line, sizeof(line), "空气AQI: %3.0f", data.mq135AqiEstimate);
  } else {
    snprintf(line, sizeof(line), "空气:预热 %4u", data.mq135Raw);
  }
  u8g2_DrawUTF8(&oled, 2, 38, line);
  drawAlertMark(38, status.airAlert);

  // DHT11 本轮读取失败时用占位符；不会画出上一轮缓存的数据。
  if (data.dhtValid) snprintf(line, sizeof(line), "温度: %4.1fC", data.temperatureC);
  else snprintf(line, sizeof(line), "温度: --.-C");
  u8g2_DrawUTF8(&oled, 2, 50, line);
  drawAlertMark(50, status.temperatureAlert);

  if (data.dhtValid) snprintf(line, sizeof(line), "湿度: %4.0f%%", data.humidityPercent);
  else snprintf(line, sizeof(line), "湿度: --%%");
  u8g2_DrawUTF8(&oled, 2, 62, line);
  drawAlertMark(62, status.humidityAlert);

  u8g2_SendBuffer(&oled);
}

void drawSettings(const Thresholds& thresholds, uint8_t selectedIndex, bool editing,
                  const char* ipAddress, bool stationConnected,
                  const ReportUploadStatus& reportStatus, bool showNetworkInfo) {
  // 这里使用局部快照数组，仅用于把结构体按固定菜单顺序渲染；修改操作
  // 仍由 main.cpp 统一完成，避免显示层拥有业务状态。
  const char* labels[] = {"光照下限", "空气", "温度", "湿度"};
  const float values[] = {thresholds.lightPercent, thresholds.mq135Aqi,
                          thresholds.temperatureC, thresholds.humidityPercent};
  const char* units[] = {"%", "AQI", "C", "%"};
  char line[28];

  u8g2_ClearBuffer(&oled);
  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);
  u8g2_DrawBox(&oled, 0, 0, 128, 14);
  u8g2_SetDrawColor(&oled, 0);
  const char* title = showNetworkInfo ? "网络信息  向上返回" :
                      (editing ? "调节阈值  短按确认" : "选择项目  短按调节");
  u8g2_DrawUTF8(&oled, 2, 12, title);
  u8g2_SetDrawColor(&oled, 1);

  if (showNetworkInfo) {
    char elapsed[16];
    const auto formatElapsed = [](bool available, uint32_t timestamp, char* output,
                                  size_t outputSize) {
      if (!available) {
        snprintf(output, outputSize, "无记录");
        return;
      }
      const uint32_t seconds = (millis() - timestamp) / 1000;
      if (seconds < 5) snprintf(output, outputSize, "刚刚");
      else if (seconds < 60) snprintf(output, outputSize, "%lu秒前",
                                     static_cast<unsigned long>(seconds));
      else if (seconds < 3600) snprintf(output, outputSize, "%lu分前",
                                       static_cast<unsigned long>(seconds / 60));
      else snprintf(output, outputSize, "%lu时前",
                    static_cast<unsigned long>(seconds / 3600));
    };

    u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);
    snprintf(line, sizeof(line), "IP: %s", stationConnected ? ipAddress : "未连接");
    u8g2_DrawUTF8(&oled, 3, 26, line);

    formatElapsed(reportStatus.hasDataUpload, reportStatus.lastDataUploadMs,
                  elapsed, sizeof(elapsed));
    snprintf(line, sizeof(line), "数据上报: %s", elapsed);
    u8g2_DrawUTF8(&oled, 3, 38, line);

    formatElapsed(reportStatus.hasHeartbeatUpload, reportStatus.lastHeartbeatUploadMs,
                  elapsed, sizeof(elapsed));
    snprintf(line, sizeof(line), "心跳上报: %s", elapsed);
    u8g2_DrawUTF8(&oled, 3, 50, line);

    const char* apiText = "待检测";
    if (reportStatus.apiConnectivity == ApiConnectivity::NotConfigured) apiText = "未配置";
    else if (!stationConnected ||
             reportStatus.apiConnectivity == ApiConnectivity::Unreachable) apiText = "不通";
    else if (reportStatus.apiConnectivity == ApiConnectivity::Reachable) apiText = "连通";
    snprintf(line, sizeof(line), "API: %s", apiText);
    u8g2_DrawUTF8(&oled, 3, 62, line);
    u8g2_SendBuffer(&oled);
    return;
  }

  for (uint8_t row = 0; row < 4; ++row) {
    // 基线从 26 到 62，每行相隔 12 像素，与 wqy12 字体高度匹配。
    const uint8_t y = 26 + row * 12;
    const uint8_t thresholdIndex = row;
    if (thresholdIndex == 2) {
      snprintf(line, sizeof(line), "%s: %4.1f%s", labels[thresholdIndex],
               values[thresholdIndex], units[thresholdIndex]);
    } else {
      snprintf(line, sizeof(line), "%s: %4.0f%s", labels[thresholdIndex],
               values[thresholdIndex], units[thresholdIndex]);
    }
    if (thresholdIndex == selectedIndex) {
      if (editing) {
        // 实心反白代表“旋转会立即改数值”；边框仅代表当前光标位置。
        u8g2_DrawBox(&oled, 0, y - 10, 128, 12);
        u8g2_SetDrawColor(&oled, 0);
        u8g2_DrawUTF8(&oled, 3, y, line);
        u8g2_SetDrawColor(&oled, 1);
      } else {
        u8g2_DrawFrame(&oled, 0, y - 10, 128, 12);
        u8g2_DrawUTF8(&oled, 3, y, line);
      }
    } else {
      u8g2_DrawUTF8(&oled, 3, y, line);
    }
  }
  u8g2_SendBuffer(&oled);
}
