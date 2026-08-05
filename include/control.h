/** @file control.h - EC11 rotary encoder input. */
#pragma once

#include <Arduino.h>

namespace Pins {
// 与 README 接线表保持一致；不要把这三个引脚分散写在业务文件中。
constexpr uint8_t kEncoderClk = 4;
constexpr uint8_t kEncoderDt = 5;
constexpr uint8_t kEncoderSw = 8;
}  // namespace Pins

void controlInit();                  // 配置引脚并注册 CLK 上升沿中断。
void controlUpdate();                // 主循环每轮调用，处理 SW 消抖与按压事件。
int controlReadEncoderDelta();       // 取走并清零 ISR 累计的旋转方向。
bool controlWasShortPress();
// 持续按下达到阈值时立即产生一次事件，不需要等到松手。
bool controlWasLongPress();
