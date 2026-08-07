/** @file buzzer.cpp */
#include "buzzer.h"

#include <Preferences.h>

namespace {
constexpr uint32_t kBuzzerOnMs = 50;
constexpr uint16_t kBuzzerFrequencyHz = 2400;
constexpr char kPreferencesNamespace[] = "buzzer_cfg";
constexpr char kEnabledKey[] = "enabled";

bool buzzerOn = false;
bool toneStarted = false;
bool enabled = true;
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
  Preferences preferences;
  if (preferences.begin(kPreferencesNamespace, false)) {
    enabled = preferences.getBool(kEnabledKey, true);
    preferences.end();
  } else {
    enabled = true;
    Serial.println("[BUZZER] ERROR: failed to open NVS; using enabled default");
  }
  Serial.printf("[BUZZER] initialized gpio=%u type=passive trigger=active-low frequency=%uHz beep=%lums enabled=%u\r\n",
                Pins::kBuzzer, kBuzzerFrequencyHz,
                static_cast<unsigned long>(kBuzzerOnMs), enabled ? 1 : 0);
}

void buzzerUpdate(uint8_t activeAlertMask) {
  const uint32_t now = millis();
  const uint8_t newlyActiveAlerts =
      activeAlertMask & static_cast<uint8_t>(~previousAlertMask);
  previousAlertMask = activeAlertMask;

  if (!enabled) {
    if (buzzerOn) setBuzzer(false);
    return;
  }

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

bool buzzerEnabled() { return enabled; }

bool buzzerSetEnabled(bool nextEnabled) {
  if (nextEnabled == enabled) return true;

  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    Serial.println("[BUZZER] ERROR: failed to open NVS while saving enabled state");
    return false;
  }
  const bool saved = preferences.putBool(kEnabledKey, nextEnabled) == 1;
  preferences.end();
  if (!saved) {
    Serial.println("[BUZZER] ERROR: failed to save enabled state");
    return false;
  }

  enabled = nextEnabled;
  if (!enabled) setBuzzer(false);
  Serial.printf("[BUZZER] enabled=%u saved to NVS\r\n", enabled ? 1 : 0);
  return true;
}
