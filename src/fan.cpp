#include "fan.h"

#include <driver/mcpwm.h>

namespace {
constexpr mcpwm_unit_t kFanUnit = MCPWM_UNIT_0;
constexpr mcpwm_timer_t kFanTimer = MCPWM_TIMER_1;
constexpr mcpwm_generator_t kFanGeneratorA = MCPWM_GEN_A;
constexpr mcpwm_generator_t kFanGeneratorB = MCPWM_GEN_B;
constexpr uint32_t kFanFrequencyHz = 20000;
bool ready = false;
bool manual = false;
bool automaticNeedsApply = false;
bool running = false;
int8_t direction = 1;
uint8_t speed = 0;
uint32_t lastLogMs = 0;

void apply(int8_t nextDirection, uint8_t nextSpeed) {
  if (!ready) return;
  nextDirection = nextDirection < 0 ? -1 : 1;
  nextSpeed = constrain(nextSpeed, static_cast<uint8_t>(0), static_cast<uint8_t>(100));
  // This board's L9110H wiring has IN2 as the forward input and IN1 as reverse.
  const float forwardDuty = nextDirection > 0 ? static_cast<float>(nextSpeed) : 0.0f;
  const float reverseDuty = nextDirection < 0 ? static_cast<float>(nextSpeed) : 0.0f;
  const esp_err_t a = mcpwm_set_duty(kFanUnit, kFanTimer, kFanGeneratorA, reverseDuty);
  const esp_err_t b = mcpwm_set_duty(kFanUnit, kFanTimer, kFanGeneratorB, forwardDuty);
  if (a != ESP_OK || b != ESP_OK) {
    Serial.printf("[FAN] PWM write failed dir=%d speed=%u err=%d/%d\r\n", nextDirection, nextSpeed,
                  static_cast<int>(a), static_cast<int>(b));
    return;
  }
  direction = nextDirection;
  speed = nextSpeed;
  running = nextSpeed != 0;
  Serial.printf("[FAN] pwm direction=%d speed=%u%% dutyA=%.0f dutyB=%.0f\r\n",
                direction, speed, reverseDuty, forwardDuty);
}
}  // namespace

void fanInit() {
  const esp_err_t gpioA = mcpwm_gpio_init(kFanUnit, MCPWM1A, Pins::kFanIn1);
  const esp_err_t gpioB = mcpwm_gpio_init(kFanUnit, MCPWM1B, Pins::kFanIn2);
  mcpwm_config_t config{};
  config.frequency = kFanFrequencyHz;
  config.cmpr_a = 0;
  config.cmpr_b = 0;
  config.duty_mode = MCPWM_DUTY_MODE_0;
  config.counter_mode = MCPWM_UP_COUNTER;
  const esp_err_t initResult = (gpioA == ESP_OK && gpioB == ESP_OK)
                                   ? mcpwm_init(kFanUnit, kFanTimer, &config)
                                   : (gpioA != ESP_OK ? gpioA : gpioB);
  if (initResult != ESP_OK) {
    Serial.printf("[FAN] ERROR: L9110H MCPWM init failed IN1=%u IN2=%u err=%d\r\n",
                  Pins::kFanIn1, Pins::kFanIn2, static_cast<int>(initResult));
    return;
  }
  ready = true;
  apply(1, 0);
  Serial.printf("[FAN] initialized L9110H IN1=%u IN2=%u pwm=%luHz auto=any-of-3\r\n",
                Pins::kFanIn1, Pins::kFanIn2, static_cast<unsigned long>(kFanFrequencyHz));
}

void fanUpdate(const EnvironmentStatus& status) {
  if (!ready || manual) return;
  const bool shouldRun = status.airAlert || status.temperatureAlert || status.humidityAlert;
  const uint8_t desiredSpeed = shouldRun ? 100 : 0;
  const uint32_t now = millis();
  if (now - lastLogMs >= 2000) {
    Serial.printf("[FAN] air=%u temp=%u humidity=%u target=%s dir=%d speed=%u%%\r\n",
                  status.airAlert ? 1 : 0, status.temperatureAlert ? 1 : 0,
                  status.humidityAlert ? 1 : 0, shouldRun ? "ON" : "OFF", direction, desiredSpeed);
    lastLogMs = now;
  }
  if (automaticNeedsApply || desiredSpeed != speed) {
    apply(direction, desiredSpeed);
    automaticNeedsApply = false;
  }
}

bool fanSetManual(int8_t nextDirection, uint8_t speedPercent) {
  if (!ready || nextDirection == 0 || speedPercent > 100) return false;
  manual = true;
  apply(nextDirection, speedPercent);
  Serial.printf("[FAN] manual direction=%d speed=%u%%\r\n", nextDirection, speedPercent);
  return true;
}

void fanResumeAutomatic() {
  manual = false;
  automaticNeedsApply = true;
  Serial.println("[FAN] automatic threshold control resumed");
}

bool fanReady() { return ready; }
bool fanManualMode() { return manual; }
bool fanRunning() { return running; }
int8_t fanDirection() { return direction; }
uint8_t fanSpeedPercent() { return speed; }
