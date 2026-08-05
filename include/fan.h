/** @file fan.h */
#pragma once

#include <Arduino.h>

#include "environment_status.h"

namespace Pins {
// L9110H IN1/IN2. These pins avoid the RTC bus, servo, PSRAM and native USB pins.
constexpr uint8_t kFanIn1 = 41;
constexpr uint8_t kFanIn2 = 42;
}  // namespace Pins

void fanInit();
void fanUpdate(const EnvironmentStatus& status);
bool fanSetManual(int8_t direction, uint8_t speedPercent);
void fanResumeAutomatic();
bool fanReady();
bool fanManualMode();
bool fanRunning();
int8_t fanDirection();
uint8_t fanSpeedPercent();
