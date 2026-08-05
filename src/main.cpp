/** @file main.cpp
 * @brief 程序入口：读取传感器、处理交互判断，并调度各功能模块。
 */
#include <Arduino.h>
#include <esp_log.h>

#include "SSD1306.h"
#include "buzzer.h"
#include "control.h"
#include "environment_status.h"
#include "fan.h"
#include "network_portal.h"
#include "rtc_clock.h"
#include "sensors.h"
#include "servo.h"
#include "threshold_store.h"

namespace {
enum class Screen : uint8_t { Monitor, Settings };

SensorManager sensors;
NetworkPortal networkPortal;
Thresholds thresholds;
Screen screen = Screen::Monitor;
uint8_t selectedThreshold = 0;
bool editingThreshold = false;
bool thresholdsDirty = false;
bool settingsNetworkInfoVisible = false;

// 设置页的编辑判断留在 main：这里决定“旋转”是在移动光标还是修改阈值；
// 阈值的保存细节则封装在 threshold_store 模块中。
void adjustSelectedThreshold(int direction) {
  switch (selectedThreshold) {
    case 0:
      thresholds.lightPercent = constrain(thresholds.lightPercent + direction, 0.0f, 100.0f);
      break;
    case 1:
      thresholds.mq135Aqi = constrain(thresholds.mq135Aqi + direction * 5.0f, 0.0f, 500.0f);
      break;
    case 2:
      thresholds.temperatureC = constrain(thresholds.temperatureC + direction * 0.5f, 0.0f, 60.0f);
      break;
    case 3:
      thresholds.humidityPercent = constrain(thresholds.humidityPercent + direction, 0.0f, 100.0f);
      break;
  }
  thresholdsDirty = true;
}

void updateSettings(int direction) {
  if (editingThreshold) {
    if (direction != 0) adjustSelectedThreshold(direction);
    if (controlWasShortPress()) {
      if (thresholdsDirty && thresholdStoreSave(thresholds)) thresholdsDirty = false;
      editingThreshold = false;
    }
    return;
  }

  if (direction > 0) {
    if (settingsNetworkInfoVisible) settingsNetworkInfoVisible = false;
    else if (selectedThreshold > 0) --selectedThreshold;
  } else if (direction < 0) {
    if (selectedThreshold < 3) ++selectedThreshold;
    else settingsNetworkInfoVisible = true;
  }
  // 网络信息页只读；显示它时短按不进入任何编辑状态。
  if (controlWasShortPress() && !settingsNetworkInfoVisible) editingThreshold = true;
  if (controlWasLongPress()) {
    screen = Screen::Monitor;
    settingsNetworkInfoVisible = false;
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  // 项目使用开发板 USB-UART 桥接串口；与 ROM 启动日志保持同一端口。
  // 短暂等待不会阻塞 UART，可给串口监视器留出复位后的连接时间。
  const uint32_t serialReadyDeadline = millis() + 1500;
  while (!Serial && millis() < serialReadyDeadline) delay(10);
  Serial.setDebugOutput(true);
  esp_log_level_set("*", ESP_LOG_INFO);
  delay(100);
  Serial.println("\n[boot] Classroom ESP32 monitor");
  Serial.printf("[MEM] flash=%uMB psram=%s psram_size=%uMB\r\n",
                static_cast<unsigned int>(ESP.getFlashChipSize() / (1024 * 1024)),
                psramFound() ? "yes" : "no",
                static_cast<unsigned int>(ESP.getPsramSize() / (1024 * 1024)));
  // 低电平触发蜂鸣器必须尽早拉高，避免其在 Wi-Fi/OLED 初始化期间误响。
  buzzerInit();

  thresholdStoreBegin(thresholds);
  oledInit();
  drawBootScreen();
  rtcClock.begin();
  servoInit();
  fanInit();
  networkPortal.begin();
  controlInit();
  sensors.begin();
  delay(800);
}

void loop() {
  // 传感器读取由 sensors.cpp 实现；主循环负责何时取样和如何使用读数。
  sensors.update();
  controlUpdate();
  networkPortal.update(sensors.data());
  const EnvironmentStatus environmentStatus = evaluateEnvironment(sensors.data(), thresholds);
  const uint8_t environmentAlertMask =
      (environmentStatus.light1Alert ? 0x01 : 0x00) |
      (environmentStatus.light2Alert ? 0x02 : 0x00) |
      (environmentStatus.airAlert ? 0x04 : 0x00) |
      (environmentStatus.temperatureAlert ? 0x08 : 0x00) |
      (environmentStatus.humidityAlert ? 0x10 : 0x00);
  buzzerUpdate(environmentAlertMask);
  servoUpdate(environmentStatus);
  fanUpdate(environmentStatus);

  // 无论配网前用户停在何种设置状态，联网成功后都回到实时监测页。
  if (networkPortal.takeConnectionSucceeded()) {
    screen = Screen::Monitor;
    editingThreshold = false;
    settingsNetworkInfoVisible = false;
  }

  // 首次配网与连接过程由网络模块接管 OLED；连接成功后自动恢复监测页面。
  if (networkPortal.isShowingNetworkScreen()) {
    delay(15);
    return;
  }

  const int rawDelta = controlReadEncoderDelta();
  const int direction = rawDelta > 0 ? 1 : (rawDelta < 0 ? -1 : 0);
  if (screen == Screen::Monitor) {
    if (controlWasLongPress()) {
      screen = Screen::Settings;
      selectedThreshold = 0;
      editingThreshold = false;
      settingsNetworkInfoVisible = false;
    }
    // 超限规则独立在 environment_status.cpp，main 决定在监测页使用该判断结果。
    drawMonitor(sensors.data(), environmentStatus);
  } else {
    const bool stationConnected = networkPortal.isStationConnected();
    updateSettings(direction);
    const String ipAddress = networkPortal.stationIpAddress();
    const ReportUploadStatus reportStatus = networkPortal.reportUploadStatus();
    drawSettings(thresholds, selectedThreshold, editingThreshold,
                 ipAddress.c_str(), stationConnected, reportStatus,
                 settingsNetworkInfoVisible);
  }

  delay(15);
}
