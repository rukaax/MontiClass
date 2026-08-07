/**
 * @file lighting.h
 * @brief 三路 CJ3400 低侧 NMOS LED 的引脚和控制接口声明。
 *
 * LEDC 初始化、PWM 输出和手动覆盖实现集中在 src/main.cpp。
 */
#pragma once

#include <Arduino.h>

namespace Pins {
constexpr uint8_t kLed1 = 11;  // LED1：跟随光敏模块 1
constexpr uint8_t kLed2 = 12;  // LED2：跟随光敏模块 2
constexpr uint8_t kLed3 = 13;  // LED3：跟随光敏模块 2
}  // namespace Pins

void lightingInit();
// main.cpp 已经计算三路亮度；本函数只在非手动模式下执行目标值。
void lightingApplyAutomatic(float led1Percent, float led2Percent, float led3Percent);
bool lightingSetManual(uint8_t led1Percent, uint8_t led2Percent, uint8_t led3Percent);
void lightingResumeAutomatic();
bool lightingReady();
bool lightingManualMode();
uint8_t lightingBrightnessPercent(uint8_t ledIndex);
