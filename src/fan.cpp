#include "fan.h"

#include <driver/mcpwm.h>

namespace {
// Keep the fan on the second MCPWM group so it cannot reconfigure the servo timer.
constexpr mcpwm_unit_t kFanUnit = MCPWM_UNIT_1;
constexpr mcpwm_timer_t kFanTimer = MCPWM_TIMER_0;
constexpr mcpwm_generator_t kFanGeneratorA = MCPWM_GEN_A;
constexpr mcpwm_generator_t kFanGeneratorB = MCPWM_GEN_B;
constexpr uint32_t kFanFrequencyHz = 20000;
// 直流风扇在低占空比下只会嗡嗡响而不转，所以一旦需要换气就至少给到起转档。
constexpr uint8_t kMinRunSpeedPercent = 40;
constexpr uint8_t kMaxSpeedPercent = 100;
// 迟滞：降速比升速迟钝，避免读数在阈值附近抖动时风扇忽快忽慢。
constexpr uint8_t kSpeedDeadbandPercent = 5;
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

// AQI 与温湿度共同决定转速：取三者中最紧迫的一项作为主负荷，
// 另外两项按较小权重叠加，这样"单项严重超标"和"三项同时轻微超标"都能提速。
uint8_t blendSpeed(const EnvironmentStatus& status) {
  const float loads[3] = {status.airLoad, status.temperatureLoad, status.humidityLoad};
  float dominant = 0.0f;
  float total = 0.0f;
  uint8_t activeCount = 0;
  for (const float load : loads) {
    if (load > dominant) dominant = load;
    total += load;
    if (load > 0.0f) ++activeCount;
  }
  if (activeCount == 0) return 0;
  // dominant 决定基础档位，其余负荷贡献剩余的一半，合计封顶在 1。
  const float secondary = total - dominant;
  float blended = dominant + secondary * 0.5f;
  if (blended > 1.0f) blended = 1.0f;
  const float range = static_cast<float>(kMaxSpeedPercent - kMinRunSpeedPercent);
  return static_cast<uint8_t>(kMinRunSpeedPercent + blended * range + 0.5f);
}
}  // namespace

void fanInit() {
  const esp_err_t gpioA = mcpwm_gpio_init(kFanUnit, MCPWM0A, Pins::kFanIn1);
  const esp_err_t gpioB = mcpwm_gpio_init(kFanUnit, MCPWM0B, Pins::kFanIn2);
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
  Serial.printf("[FAN] initialized L9110H IN1=%u IN2=%u pwm=%luHz auto=blend(aqi,temp,humidity) speed=%u..%u%%\r\n",
                Pins::kFanIn1, Pins::kFanIn2, static_cast<unsigned long>(kFanFrequencyHz),
                kMinRunSpeedPercent, kMaxSpeedPercent);
}

void fanUpdate(const EnvironmentStatus& status) {
  if (!ready || manual) return;
  const uint8_t desiredSpeed = blendSpeed(status);
  const uint32_t now = millis();
  if (now - lastLogMs >= 2000) {
    Serial.printf("[FAN] load air=%.2f temp=%.2f humidity=%.2f target=%u%% dir=%d speed=%u%%\r\n",
                  status.airLoad, status.temperatureLoad, status.humidityLoad, desiredSpeed,
                  direction, speed);
    lastLogMs = now;
  }
  // 起停必须立刻响应；已在运转时只有变化超过死区才重写占空比。
  const bool crossesStopBoundary = (desiredSpeed == 0) != (speed == 0);
  const uint8_t delta = desiredSpeed > speed ? desiredSpeed - speed : speed - desiredSpeed;
  if (automaticNeedsApply || crossesStopBoundary || delta >= kSpeedDeadbandPercent) {
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
