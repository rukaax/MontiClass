/** @file buzzer.cpp */
#include "buzzer.h"

namespace {
constexpr uint32_t kBuzzerOnMs = 50;
constexpr uint16_t kBuzzerFrequencyHz = 2400;

bool buzzerOn = false;
bool toneStarted = false;
uint8_t previousAlertMask = 0;
uint32_t beepStartedMs = 0;

void setBuzzer(bool on) {
  buzzerOn = on;
  if (on) {
    // 无源蜂鸣器需要方波；低电平触发只影响静音电平，方波相位不影响音调。
    tone(Pins::kBuzzer, kBuzzerFrequencyHz);
    toneStarted = true;
  } else {
    // Arduino-ESP32 reports an LEDC error if noTone() is called before tone()
    // has allocated its channel during startup.
    if (toneStarted) {
      noTone(Pins::kBuzzer);
      toneStarted = false;
    }
    // 模块低电平触发，停止 PWM 后必须保持高电平，不能让输入悬空或拉低。
    digitalWrite(Pins::kBuzzer, HIGH);
  }
}
}  // namespace

void buzzerInit() {
  // 先把输出锁存器置为高，再切为 OUTPUT，避免低电平触发模块在启动时误响。
  digitalWrite(Pins::kBuzzer, HIGH);
  pinMode(Pins::kBuzzer, OUTPUT);
  buzzerOn = false;
  Serial.printf("[BUZZER] initialized gpio=%u type=passive trigger=active-low frequency=%uHz beep=%lums\r\n",
                Pins::kBuzzer, kBuzzerFrequencyHz,
                static_cast<unsigned long>(kBuzzerOnMs));
}

void buzzerUpdate(uint8_t activeAlertMask) {
  const uint32_t now = millis();
  const uint8_t newlyActiveAlerts =
      activeAlertMask & static_cast<uint8_t>(~previousAlertMask);
  previousAlertMask = activeAlertMask;

  if (newlyActiveAlerts != 0) {
    beepStartedMs = now;
    setBuzzer(true);
    Serial.printf("[BUZZER] threshold crossed: new=0x%02X active=0x%02X\r\n",
                  newlyActiveAlerts, activeAlertMask);
  }

  if (buzzerOn && now - beepStartedMs >= kBuzzerOnMs) {
    setBuzzer(false);
    Serial.println("[BUZZER] one-shot beep completed");
  }
}
