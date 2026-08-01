/** @file SSD1306.h */
#pragma once

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "environment_status.h"
#include "sensors.h"
#include "thresholds.h"

namespace Pins {
// OLED 使用原工程指定的硬件 I2C 引脚，不使用开发板默认引脚。
constexpr uint8_t kOledSda = 17;
constexpr uint8_t kOledScl = 18;
}
constexpr uint8_t kOledAddress = 0x3C;  // 常见 SSD1306 的 7 位 I2C 地址。

void oledInit();  // 初始化 I2C 与 SSD1306；必须在任何 draw* 之前调用。
void drawBootScreen();
// 无线网络尚未配置时显示热点名称、无密码提示、入口二维码和 OLED 暂不连接选项。
void drawProvisioningScreen(const char* ssid, const char* qrPayload);
// 已提交凭据、正在连接路由器时显示状态，避免被监测页面覆盖。
void drawNetworkConnectingScreen(const char* ssid);
// 监测页同时显示四类数据；告警状态由环境判断模块提前计算。
void drawMonitor(const SensorData& data, const EnvironmentStatus& status);
// selectedIndex 的范围为 0..3；editing 决定当前行是边框还是反白。
void drawSettings(const Thresholds& thresholds, uint8_t selectedIndex, bool editing);
