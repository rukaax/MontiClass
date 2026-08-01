/** @file main.cpp
 * @brief 程序入口：读取传感器、处理交互判断，并调度各功能模块。
 */
#include <Arduino.h>

#include "SSD1306.h"
#include "control.h"
#include "environment_status.h"
#include "network_portal.h"
#include "sensors.h"
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

// 设置页的编辑判断留在 main：这里决定“旋转”是在移动光标还是修改阈值；
// 阈值的保存细节则封装在 threshold_store 模块中。
void adjustSelectedThreshold(int direction) {
  switch (selectedThreshold) {
    case 0:
      thresholds.lightPercent = constrain(thresholds.lightPercent + direction, 0.0f, 100.0f);
      break;
    case 1:
      thresholds.mq135Ppm = constrain(thresholds.mq135Ppm + direction * 10.0f, 0.0f, 2000.0f);
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

  if (direction > 0 && selectedThreshold > 0) --selectedThreshold;
  if (direction < 0 && selectedThreshold < 3) ++selectedThreshold;
  if (controlWasShortPress()) editingThreshold = true;
  if (controlWasLongPress()) screen = Screen::Monitor;
}
}  // namespace

void setup() {
  Serial.begin(115200);
  // ESP32-S3 原生 USB CDC 在开机瞬间可能尚未被电脑枚举，短暂等待可避免
  // 丢失第一批启动日志；UART 模式下该循环会立刻通过。
  const uint32_t serialReadyDeadline = millis() + 1500;
  while (!Serial && millis() < serialReadyDeadline) delay(10);
  Serial.setDebugOutput(true);
  delay(100);
  Serial.println("\n[boot] Classroom ESP32 monitor");

  thresholdStoreBegin(thresholds);
  oledInit();
  drawBootScreen();
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

  // 无论配网前用户停在何种设置状态，联网成功后都回到实时监测页。
  if (networkPortal.takeConnectionSucceeded()) {
    screen = Screen::Monitor;
    editingThreshold = false;
  }

  networkPortal.handleProvisioningInput(controlWasShortPress());

  // 首次配网与连接过程由网络模块接管 OLED；连接成功后自动恢复监测页面。
  if (networkPortal.isShowingNetworkScreen()) {
    controlDebugPrint();
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
    }
    // 超限规则独立在 environment_status.cpp，main 决定在监测页使用该判断结果。
    drawMonitor(sensors.data(), evaluateEnvironment(sensors.data(), thresholds));
  } else {
    updateSettings(direction);
    drawSettings(thresholds, selectedThreshold, editingThreshold);
  }

  controlDebugPrint();
  delay(15);
}
