/**
 * @file servo.h
 * @brief SG90 舵机引脚和控制接口声明。
 *
 * MCPWM 初始化、角度输出和 WebUI 手动覆盖实现集中在 src/main.cpp。
 */
#pragma once

#include <Arduino.h>

namespace Pins {
// GPIO21 不属于 OPI PSRAM 引脚 GPIO33..37，也不占用 USB 的 GPIO19/20。
constexpr uint8_t kServo = 21;
}  // namespace Pins

void servoInit();
// main.cpp 已经决定目标角度；本函数只在非手动模式下写入该角度。
void servoApplyAutomaticAngle(uint16_t angle);
bool servoSetManualAngle(uint16_t angle);
void servoResumeAutomatic();
bool servoReady();
bool servoManualMode();
uint16_t servoCurrentAngle();
