/**
 * @file fan.h
 * @brief L9110H 风扇引脚和控制接口声明。
 *
 * MCPWM 初始化、输出和 WebUI 手动覆盖实现集中在 src/main.cpp。
 */
#pragma once

#include <Arduino.h>

namespace Pins {
// GPIO41/42 避开 RTC、舵机、OPI PSRAM 和原生 USB 使用的引脚。
constexpr uint8_t kFanIn1 = 41;
constexpr uint8_t kFanIn2 = 42;
}  // namespace Pins

void fanInit();
// 自动模式运行中每100ms向目标调整1%，目标上下2%内保持；起停单独处理。
void fanApplyAutomatic(int8_t direction, uint8_t speedPercent);
bool fanSetManual(int8_t direction, uint8_t speedPercent);
void fanResumeAutomatic();
bool fanReady();
bool fanManualMode();
bool fanRunning();
int8_t fanDirection();
uint8_t fanSpeedPercent();
