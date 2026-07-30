/** @file SSD1306.h */
#pragma once

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "sensors.h"

namespace Pins {
// OLED 使用原工程指定的硬件 I2C 引脚，不使用开发板默认引脚。
constexpr uint8_t kOledSda = 17;
constexpr uint8_t kOledScl = 18;
}
constexpr uint8_t kOledAddress = 0x3C;  // 常见 SSD1306 的 7 位 I2C 地址。

struct Thresholds {
  // 四项默认值与设置页的菜单顺序保持一致，单位见字段名。
  float lightPercent = 50.0f;
  float mq135Ppm = 500.0f;
  float temperatureC = 30.0f;
  float humidityPercent = 60.0f;
};

void oledInit();  // 初始化 I2C 与 SSD1306；必须在任何 draw* 之前调用。
void drawBootScreen();
// 监测页同时显示四类数据；阈值超出时在对应行末画圆点。
void drawMonitor(const SensorData& data, const Thresholds& thresholds);
// selectedIndex 的范围为 0..3；editing 决定当前行是边框还是反白。
void drawSettings(const Thresholds& thresholds, uint8_t selectedIndex, bool editing);
