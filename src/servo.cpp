#include "servo.h"

#include <driver/mcpwm.h>

namespace {
constexpr mcpwm_unit_t kServoUnit = MCPWM_UNIT_0;
constexpr mcpwm_timer_t kServoTimer = MCPWM_TIMER_0;
constexpr mcpwm_generator_t kServoGenerator = MCPWM_GEN_A;
constexpr uint32_t kServoFrequencyHz = 50;
constexpr uint16_t kMinPulseUs = 500;
constexpr uint16_t kMaxPulseUs = 2500;
bool servoAtOpenPosition = false;
bool servoPwmReady = false;
bool servoManualOverride = false;
bool servoAutomaticNeedsApply = false;
uint16_t currentServoAngle = 0;
uint32_t lastServoLogMs = 0;

void writeServoAngle(uint16_t angle) {
  if (!servoPwmReady) return;
  angle = constrain(angle, static_cast<uint16_t>(0), static_cast<uint16_t>(180));
  const uint16_t pulseUs = static_cast<uint16_t>(
      kMinPulseUs + (static_cast<uint32_t>(kMaxPulseUs - kMinPulseUs) * angle) / 180U);
  const esp_err_t result = mcpwm_set_duty_in_us(kServoUnit, kServoTimer, kServoGenerator, pulseUs);
  if (result != ESP_OK) {
    Serial.printf("[SERVO] PWM write failed angle=%u pulse=%uus err=%d\r\n", angle, pulseUs,
                  static_cast<int>(result));
  } else {
    currentServoAngle = angle;
  }
}
}  // namespace

void servoInit() {
  pinMode(Pins::kServo, OUTPUT);
  const esp_err_t gpioResult = mcpwm_gpio_init(kServoUnit, MCPWM0A, Pins::kServo);
  mcpwm_config_t config{};
  config.frequency = kServoFrequencyHz;
  config.cmpr_a = 7.5f;
  config.cmpr_b = 0.0f;
  config.duty_mode = MCPWM_DUTY_MODE_0;
  config.counter_mode = MCPWM_UP_COUNTER;
  const esp_err_t initResult = gpioResult == ESP_OK ? mcpwm_init(kServoUnit, kServoTimer, &config) : gpioResult;
  if (initResult != ESP_OK) {
    Serial.printf("[SERVO] ERROR: MCPWM init failed gpio=%u err=%d\r\n", Pins::kServo,
                  static_cast<int>(initResult));
    return;
  }
  servoPwmReady = true;
  writeServoAngle(0);
  servoAtOpenPosition = false;
  Serial.printf("[SERVO] initialized gpio=%u MCPWM=0A pwm=%uHz pulse=500..2500us range=0..180deg trigger=2-of-3\r\n",
                Pins::kServo, static_cast<unsigned int>(kServoFrequencyHz));
}

void servoUpdate(const EnvironmentStatus& status) {
  if (!servoPwmReady) return;
  if (servoManualOverride) {
    const uint32_t now = millis();
    if (now - lastServoLogMs >= 2000) {
      Serial.printf("[SERVO] manual mode angle=%u deg\r\n", currentServoAngle);
      lastServoLogMs = now;
    }
    return;
  }
  const uint8_t exceeded = (status.airAlert ? 1 : 0) +
                           (status.temperatureAlert ? 1 : 0) +
                           (status.humidityAlert ? 1 : 0);
  const bool shouldOpen = exceeded >= 2;
  const uint32_t now = millis();
  if (now - lastServoLogMs >= 2000) {
    Serial.printf("[SERVO] air=%u temp=%u humidity=%u exceeded=%u/3 target=%u\r\n",
                  status.airAlert ? 1 : 0, status.temperatureAlert ? 1 : 0,
                  status.humidityAlert ? 1 : 0, exceeded, shouldOpen ? 180 : 0);
    lastServoLogMs = now;
  }
  if (!servoAutomaticNeedsApply && shouldOpen == servoAtOpenPosition) return;
  writeServoAngle(shouldOpen ? 180 : 0);
  servoAtOpenPosition = shouldOpen;
  servoAutomaticNeedsApply = false;
  Serial.printf("[SERVO] target=%u deg exceeded=%u/3\r\n", shouldOpen ? 180 : 0, exceeded);
}

bool servoSetManualAngle(uint16_t angle) {
  if (!servoPwmReady || angle > 180) return false;
  servoManualOverride = true;
  writeServoAngle(angle);
  Serial.printf("[SERVO] manual target=%u deg\r\n", angle);
  return true;
}

void servoResumeAutomatic() {
  servoManualOverride = false;
  servoAutomaticNeedsApply = true;
  Serial.println("[SERVO] automatic threshold control resumed");
}

bool servoReady() { return servoPwmReady; }

bool servoManualMode() { return servoManualOverride; }

uint16_t servoCurrentAngle() { return currentServoAngle; }
