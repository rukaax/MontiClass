/** @file servo.h */
#pragma once

#include <Arduino.h>

#include "environment_status.h"

namespace Pins {
// GPIO21 is outside the OPI PSRAM pins (GPIO33-37) and USB pins 19/20.
constexpr uint8_t kServo = 21;
}  // namespace Pins

void servoInit();
// At least two of AQI, temperature and humidity alerts drive the servo to 180°.
void servoUpdate(const EnvironmentStatus& status);
bool servoSetManualAngle(uint16_t angle);
void servoResumeAutomatic();
bool servoReady();
bool servoManualMode();
uint16_t servoCurrentAngle();
