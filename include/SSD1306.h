/** @file SSD1306.h */
#pragma once

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "environment_status.h"
#include "report_uploader.h"
#include "sensors.h"
#include "thresholds.h"

namespace Pins {
constexpr uint8_t kOledSda = 17;
constexpr uint8_t kOledScl = 18;
}
constexpr uint8_t kOledAddress = 0x3C;  // 常见 SSD1306 的 7 位 I2C 地址。

void oledInit();  // 初始化 I2C 与 SSD1306；必须在任何 draw* 之前调用。
String oledI2cDevicesJson();
void drawBootScreen();
// 无线网络尚未配置时显示热点名称、无密码提示和入口二维码。
void drawProvisioningScreen(const char* ssid, const char* qrPayload);
// 已提交凭据、正在连接路由器时显示状态，避免被监测页面覆盖。
void drawNetworkConnectingScreen(const char* ssid);
// 监测页同时显示两路光照、相对 AQI、温度和湿度。
void drawMonitor(const SensorData& data, const EnvironmentStatus& status);
// 网络信息是阈值列表下方的只读页面，不参与 selectedIndex 选择。
void drawSettings(const Thresholds& thresholds, uint8_t selectedIndex, bool editing,
                  const char* ipAddress, bool stationConnected,
                  const ReportUploadStatus& reportStatus, bool showNetworkInfo);
