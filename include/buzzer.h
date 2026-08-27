/** @file buzzer.h
 * @brief 环境超限时的非阻塞蜂鸣器报警。
 */
#pragma once

#include <Arduino.h>

namespace Pins {
constexpr uint8_t kBuzzer = 9;  // 低电平触发无源蜂鸣器模块 SIG
}

void buzzerInit();
// 每个 bit 代表一个告警项目；某 bit 从 0 变为 1 时只鸣叫一次。
void buzzerUpdate(uint8_t activeAlertMask);
// I2C 设备在连接超时后发出两声短报警。
void buzzerAlarmTwoBeep();
bool buzzerEnabled();
// 仅在 NVS 持久化成功后切换状态；关闭时立即停止当前鸣叫。
bool buzzerSetEnabled(bool enabled);
