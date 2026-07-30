/** @file SSD1306.cpp */
#include "SSD1306.h"

#include <math.h>

namespace {
// U8g2 的全缓冲对象。128x64 单色屏需要约 1 KB RAM，ESP32-S3 足够使用。
u8g2_t oled;

// NAN 与任何数比较都为 false；显式判断可确保传感器失效时不会误告警。
bool exceeds(float value, float threshold) { return !isnan(value) && value > threshold; }

void drawAlertMark(uint8_t y, bool alert) {
  if (!alert) return;
  // 固定画在右侧，不能覆盖最多约 100 像素宽的中文/数值文本。
  u8g2_DrawDisc(&oled, 122, y - 5, 3, U8G2_DRAW_ALL);
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

void drawMonitor(const SensorData& data, const Thresholds& thresholds) {
  char line[32];
  // 屏幕高度只有 64 像素：14 像素标题 + 4 行 12 像素正好容纳所有数据。
  u8g2_ClearBuffer(&oled);
  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);

  u8g2_DrawBox(&oled, 0, 0, 128, 14);
  u8g2_SetDrawColor(&oled, 0);
  // 标题按需求固定为“环境质量看板”；操作提示让位给更清晰的主标题。
  u8g2_DrawUTF8(&oled, 2, 12, "环境质量看板");
  u8g2_SetDrawColor(&oled, 1);

  snprintf(line, sizeof(line), "光照: %3.0f%%", data.lightPercent);
  u8g2_DrawUTF8(&oled, 2, 26, line);
  drawAlertMark(26, exceeds(data.lightPercent, thresholds.lightPercent));

  // 预热期间展示 raw ADC 而非不可信的 ppm；这样也能验证 MQ135 AO 接线。
  if (data.mq135WarmedUp) {
    snprintf(line, sizeof(line), "空气: %4.0fppm", data.mq135PpmEstimate);
  } else {
    snprintf(line, sizeof(line), "空气:预热 %4u", data.mq135Raw);
  }
  u8g2_DrawUTF8(&oled, 2, 38, line);
  drawAlertMark(38, data.mq135WarmedUp && exceeds(data.mq135PpmEstimate, thresholds.mq135Ppm));

  // DHT11 本轮读取失败时用占位符；不会画出上一轮缓存的数据。
  if (data.dhtValid) snprintf(line, sizeof(line), "温度: %4.1fC", data.temperatureC);
  else snprintf(line, sizeof(line), "温度: --.-C");
  u8g2_DrawUTF8(&oled, 2, 50, line);
  drawAlertMark(50, exceeds(data.temperatureC, thresholds.temperatureC));

  if (data.dhtValid) snprintf(line, sizeof(line), "湿度: %4.0f%%", data.humidityPercent);
  else snprintf(line, sizeof(line), "湿度: --%%");
  u8g2_DrawUTF8(&oled, 2, 62, line);
  drawAlertMark(62, exceeds(data.humidityPercent, thresholds.humidityPercent));

  u8g2_SendBuffer(&oled);
}

void drawSettings(const Thresholds& thresholds, uint8_t selectedIndex, bool editing) {
  // 这里使用局部快照数组，仅用于把结构体按固定菜单顺序渲染；修改操作
  // 仍由 main.cpp 统一完成，避免显示层拥有业务状态。
  const char* labels[] = {"光照", "空气", "温度", "湿度"};
  const float values[] = {thresholds.lightPercent, thresholds.mq135Ppm,
                          thresholds.temperatureC, thresholds.humidityPercent};
  const char* units[] = {"%", "ppm", "C", "%"};
  char line[28];

  u8g2_ClearBuffer(&oled);
  u8g2_SetFont(&oled, u8g2_font_wqy12_t_gb2312);
  u8g2_DrawBox(&oled, 0, 0, 128, 14);
  u8g2_SetDrawColor(&oled, 0);
  u8g2_DrawUTF8(&oled, 2, 12, editing ? "调节阈值  短按确认" : "选择项目  短按调节");
  u8g2_SetDrawColor(&oled, 1);

  for (uint8_t i = 0; i < 4; ++i) {
    // 基线从 26 到 62，每行相隔 12 像素，与 wqy12 字体高度匹配。
    const uint8_t y = 26 + i * 12;
    if (i == 2) snprintf(line, sizeof(line), "%s: %4.1f%s", labels[i], values[i], units[i]);
    else snprintf(line, sizeof(line), "%s: %4.0f%s", labels[i], values[i], units[i]);
    if (i == selectedIndex) {
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
