/**
 * @file lighting.h
 * @brief 三路 CJ3400 低侧 NMOS LED 的引脚和控制接口声明。
 *
 * LEDC 初始化、PWM 输出和手动覆盖实现集中在 src/main.cpp。
 */
#pragma once

#include <Arduino.h>

namespace Pins {
constexpr uint8_t kLed1 = 11;  // LED1：自动模式下与 LED2/3 同步
constexpr uint8_t kLed2 = 12;  // LED2：自动模式下作为三路同步亮度基准
constexpr uint8_t kLed3 = 13;  // LED3：自动模式下与 LED1/2 同步
}  // namespace Pins

void lightingInit();
// direction 取 -1/0/+1；自动模式下三路使用同一占空比同步降低、保持或提高 1%。
void lightingAdjustAutomatic(int8_t direction);
// 自动模式处于配置时段外时，立即关闭三路 LED；手动模式不受影响。
void lightingTurnOffAutomatic();
bool lightingSetManual(uint8_t led1Percent, uint8_t led2Percent, uint8_t led3Percent);
void lightingResumeAutomatic();
bool lightingReady();
bool lightingManualMode();
bool lightingOutdoorDark();
bool lightingPresenceRecent();
uint8_t lightingBrightnessPercent(uint8_t ledIndex);
