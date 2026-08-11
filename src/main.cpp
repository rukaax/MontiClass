/**
 * @file main.cpp
 * @brief 程序入口，以及传感器、LED、风扇、舵机的初始化、读取和控制实现。
 *
 * 初次阅读建议按以下顺序：
 * 1. 先看文件末尾的 setup()，了解设备上电后依次初始化哪些模块；
 * 2. 再看 loop()，了解一次主循环的“读取、判断、输出、显示”流程；
 * 3. 回到文件前半部分查看各传感器换算和自动控制公式；
 * 4. 最后查看中间的 SensorManager、lighting、fan、servo 硬件实现。
 */
#include <Arduino.h>
#include <driver/mcpwm.h>
#include <esp_log.h>
#include <math.h>

#include "buzzer.h"
#include "control.h"
#include "environment_status.h"
#include "fan.h"
#include "lighting.h"
#include "network_portal.h"
#include "oled_controller.h"
#include "rtc_clock.h"
#include "sensors.h"
#include "servo.h"
#include "threshold_store.h"

namespace {
SensorManager sensors;
NetworkPortal networkPortal;
Thresholds thresholds;
bool automaticOutdoorDark = false;
bool automaticPresenceRecent = true;
bool automaticFanPresenceRecent = true;
bool presenceTrackingStarted = false;
uint32_t lastPresenceDetectedMs = 0;

// ---------------------------------------------------------------------------
// 传感器采集所需的内部常量和换算函数
// ---------------------------------------------------------------------------
// 这些内容集中在 main.cpp，便于从 GPIO 初始化一路读到阈值判断。使用子命名
// 空间只是为了避免不同硬件出现同名变量，不会把代码拆到其他文件。
namespace SensorHardware {
// 每个 ADC 通道连续读取 8 次并取平均，降低电源纹波和瞬时噪声对显示、告警及
// 自动控制的影响。循环内没有 delay，因此不会明显拖慢 WebUI 和 OLED。
constexpr uint8_t kAdcSamples = 8;

// 当前两只光敏模块的 AO 在变暗时电压升高，所以 ADC 越大反而表示越暗。
// 如果以后某一路更换为相反输出特性的模块，只需修改对应布尔值。
constexpr bool kLight1DarkIsHigh = true;
constexpr bool kLight2DarkIsHigh = true;

struct AqiPoint {
  float rawRatio;  // 当前 ADC / 开机洁净空气基线
  float aqi;       // 映射后的相对 AQI
};

// MQ135 没有完成 R0 和气体曲线标定，因此这里不是国家标准 AQI，也不是 ppm。
// 曲线只描述当前 AO 相对本次开机基线的变化，适合趋势观察和本机告警。
constexpr AqiPoint kRelativeAqiCurve[] = {
    {0.0f, 0.0f}, {1.0f, 50.0f}, {1.25f, 100.0f}, {1.5f, 150.0f},
    {2.0f, 200.0f}, {3.0f, 300.0f}, {4.0f, 400.0f}, {5.0f, 500.0f},
};

float lightPercentFromRaw(uint16_t raw, bool darkIsHigh) {
  const float adcPercent = raw * 100.0f / 4095.0f;
  return darkIsHigh ? 100.0f - adcPercent : adcPercent;
}

float relativeAqiFromRaw(uint16_t raw, uint16_t baselineRaw) {
  // 基线为 0 时不能做除法。返回 NAN 后，main.cpp 的有效性保护会让该读数
  // 不产生告警，也不会推动风扇。
  if (baselineRaw == 0) return NAN;
  const float ratio = static_cast<float>(raw) / baselineRaw;

  // 在相邻标定点间做线性插值，避免 AQI 只能阶梯式跳变。
  for (size_t i = 1; i < sizeof(kRelativeAqiCurve) / sizeof(kRelativeAqiCurve[0]); ++i) {
    const AqiPoint& lower = kRelativeAqiCurve[i - 1];
    const AqiPoint& upper = kRelativeAqiCurve[i];
    if (ratio <= upper.rawRatio) {
      const float position = (ratio - lower.rawRatio) /
                             (upper.rawRatio - lower.rawRatio);
      return constrain(lower.aqi + position * (upper.aqi - lower.aqi),
                       0.0f, 500.0f);
    }
  }
  return 500.0f;
}
}  // namespace SensorHardware

// ---------------------------------------------------------------------------
// 三路 LED PWM 所需的内部状态
// ---------------------------------------------------------------------------
namespace LightingHardware {
// 20kHz 高于常见可闻频段，避免灯具或线路在 PWM 开关时产生明显啸叫。
// 10 位分辨率提供 0..1023 共 1024 级占空比。LED 使用 5V 电源时，为把 PWM
// 支路平均电压限制在 2.5V，实际占空比最高约为 50%。LED 仍必须有外部限流。
constexpr uint32_t kPwmFrequencyHz = 20000;
constexpr uint8_t kPwmResolutionBits = 10;
constexpr uint16_t kPwmFullScaleDuty = (1U << kPwmResolutionBits) - 1U;
constexpr uint16_t kLedSupplyMillivolts = 5000;
constexpr uint16_t kMaximumAverageMillivolts = 2500;
constexpr uint16_t kPwmMaxDuty = static_cast<uint16_t>(
    (static_cast<uint32_t>(kPwmFullScaleDuty) * kMaximumAverageMillivolts +
     kLedSupplyMillivolts / 2U) /
    kLedSupplyMillivolts);

// 蜂鸣器 tone() 固定使用 LEDC channel 0，并会重配 channel 0/1 共用的 timer 0。
// LED 使用 2/4/6，分别落在三个独立的 LEDC timer，避免任意一路重配或写入时
// 影响另外两路 PWM。
constexpr uint8_t kLed1Channel = 2;
constexpr uint8_t kLed2Channel = 4;
constexpr uint8_t kLed3Channel = 6;

bool ready = false;
bool manual = false;
uint16_t led1Duty = 0;
uint16_t led2Duty = 0;
uint16_t led3Duty = 0;
uint32_t lastLogMs = 0;
constexpr uint16_t kAutomaticDutyStep =
    (static_cast<uint32_t>(kPwmMaxDuty) + 50U) / 100U;
constexpr uint16_t kAutomaticMaxDuty = kPwmMaxDuty;

uint16_t dutyFromPercent(float brightnessPercent) {
  // 0..100% 表示允许亮度范围，100% 映射到上面的安全占空比，而不是 1023。
  // 因此自动策略和 WebUI 手动控制无法绕过 2.5V 平均电压限制。
  brightnessPercent = constrain(brightnessPercent, 0.0f, 100.0f);
  return static_cast<uint16_t>(brightnessPercent * kPwmMaxDuty / 100.0f + 0.5f);
}

void writeDuty(uint8_t channel, uint16_t nextDuty, uint16_t& currentDuty) {
  // 相同占空比不重复写寄存器，减少主循环中的无意义外设操作。
  if (nextDuty == currentDuty) return;
  ledcWrite(channel, nextDuty);
  currentDuty = nextDuty;
}

uint16_t adjustedAutomaticDuty(int8_t direction, uint16_t currentDuty) {
  if (direction > 0) {
    const uint32_t steppedDuty = currentDuty + kAutomaticDutyStep;
    return static_cast<uint16_t>(steppedDuty < kAutomaticMaxDuty
                                     ? steppedDuty
                                     : kAutomaticMaxDuty);
  }
  if (direction < 0) {
    return currentDuty > kAutomaticDutyStep
               ? static_cast<uint16_t>(currentDuty - kAutomaticDutyStep)
               : 0;
  }
  return currentDuty;
}
}  // namespace LightingHardware

// ---------------------------------------------------------------------------
// L9110H 风扇 MCPWM 所需的内部状态
// ---------------------------------------------------------------------------
namespace FanHardware {
// 风扇固定使用 MCPWM_UNIT_1，舵机使用 MCPWM_UNIT_0。两个不同频率的外设
// 分属不同单元，20kHz 风扇 PWM 不会破坏舵机的 50Hz 周期。
constexpr mcpwm_unit_t kUnit = MCPWM_UNIT_1;
constexpr mcpwm_timer_t kTimer = MCPWM_TIMER_0;
constexpr mcpwm_generator_t kGeneratorA = MCPWM_GEN_A;
constexpr mcpwm_generator_t kGeneratorB = MCPWM_GEN_B;
constexpr uint32_t kPwmFrequencyHz = 20000;

// 自动运行时每 100ms 最多改变 1%，与 LED 一样在目标上下 2% 内保持。
constexpr uint8_t kSpeedTolerancePercent = 2;
constexpr uint8_t kSpeedStepPercent = 1;
constexpr uint32_t kAutomaticStepIntervalMs = 100;
constexpr uint8_t kMinimumRunningSpeedPercent = 40;

bool ready = false;
bool manual = false;
bool automaticNeedsApply = false;
bool running = false;
int8_t direction = 1;
uint8_t speed = 0;
uint32_t lastLogMs = 0;
uint32_t lastAutomaticStepMs = 0;

void applyPwm(int8_t nextDirection, uint8_t nextSpeed) {
  if (!ready) return;
  // 停机使用 speed=0 表达，方向仍保留为 +1 或 -1，方便下次启动沿用。
  nextDirection = nextDirection < 0 ? -1 : 1;
  nextSpeed = constrain(nextSpeed, static_cast<uint8_t>(0),
                        static_cast<uint8_t>(100));

  // 当前接线中 IN2 为正转，IN1 为反转。任一方向工作时另一输入保持 0%，
  // 防止两个桥臂同时驱动。速度百分比可直接作为 MCPWM 占空比。
  const float forwardDuty = nextDirection > 0 ? static_cast<float>(nextSpeed) : 0.0f;
  const float reverseDuty = nextDirection < 0 ? static_cast<float>(nextSpeed) : 0.0f;
  const esp_err_t resultA = mcpwm_set_duty(kUnit, kTimer, kGeneratorA, reverseDuty);
  const esp_err_t resultB = mcpwm_set_duty(kUnit, kTimer, kGeneratorB, forwardDuty);
  if (resultA != ESP_OK || resultB != ESP_OK) {
    Serial.printf("[FAN] PWM write failed dir=%d speed=%u err=%d/%d\r\n",
                  nextDirection, nextSpeed, static_cast<int>(resultA),
                  static_cast<int>(resultB));
    return;
  }
  direction = nextDirection;
  speed = nextSpeed;
  running = nextSpeed != 0;
}
}  // namespace FanHardware

// ---------------------------------------------------------------------------
// SG90 舵机 MCPWM 所需的内部状态
// ---------------------------------------------------------------------------
namespace ServoHardware {
constexpr mcpwm_unit_t kUnit = MCPWM_UNIT_0;
constexpr mcpwm_timer_t kTimer = MCPWM_TIMER_0;
constexpr mcpwm_generator_t kGenerator = MCPWM_GEN_A;
constexpr uint32_t kPwmFrequencyHz = 50;
constexpr uint16_t kMinPulseUs = 500;
constexpr uint16_t kMaxPulseUs = 2500;
// 舵机安装方向与逻辑方向相反时，通过交换逻辑角度到 PWM 端点来修正；
// WebUI/OLED 仍显示逻辑角度，不影响自动开窗的 0°/180°语义。
constexpr bool kReverseDirection = true;

bool ready = false;
bool manual = false;
bool automaticNeedsApply = false;
uint16_t currentAngle = 0;

void writeAngle(uint16_t angle) {
  if (!ready) return;
  angle = constrain(angle, static_cast<uint16_t>(0), static_cast<uint16_t>(180));

  // SG90 用 20ms 周期内的高电平脉宽表示角度。本项目把 0..180°线性映射到
  // 500..2500us；安装方向反向时先交换逻辑角度。乘法使用 uint32_t，避免
  // 16 位中间结果溢出。
  const uint16_t outputAngle = kReverseDirection ? 180U - angle : angle;
  const uint16_t pulseUs = static_cast<uint16_t>(
      kMinPulseUs +
      (static_cast<uint32_t>(kMaxPulseUs - kMinPulseUs) * outputAngle) / 180U);
  const esp_err_t result = mcpwm_set_duty_in_us(kUnit, kTimer, kGenerator, pulseUs);
  if (result != ESP_OK) {
    Serial.printf("[SERVO] PWM write failed angle=%u pulse=%uus err=%d\r\n",
                  angle, pulseUs, static_cast<int>(result));
    return;
  }
  currentAngle = angle;
}
}  // namespace ServoHardware

// ---------------------------------------------------------------------------
// 传感器判断与自动控制策略
// ---------------------------------------------------------------------------
// 这一段集中保存“传感器数据应该如何影响系统”的全部业务规则。传感器模块只
// 负责采集，LED/风扇/舵机模块只负责输出，因此调整阈值含义或控制曲线时只需
// 检查 main.cpp，不会在多个硬件驱动文件中找到互相矛盾的判断。

// AQI、温度和湿度超过阈值多少时，分别视为 100% 环境负荷。负荷用于风扇
// 调速，不改变 OLED 和蜂鸣器采用的“刚越过阈值即告警”规则。
constexpr float kAirLoadSpanAqi = 100.0f;
constexpr float kTemperatureLoadSpanC = 6.0f;
constexpr float kHumidityLoadSpanPercent = 20.0f;
// 风扇最高允许 100% 占空比；最低可靠起转速度由 FanHardware 统一定义。
constexpr uint8_t kFanMaximumSpeedPercent = 100;

// 舵机自动控制只有两个位置：正常时关闭到 0°，AQI、温度、湿度三项中至少
// 两项告警时打开到 180°。手动模式仍可通过 WebUI 设置任意 0..180°。
constexpr uint16_t kServoClosedAngle = 0;
constexpr uint16_t kServoOpenAngle = 180;

bool isAboveUpperLimit(float value, float threshold, bool sensorValid) {
  // NAN/Inf 不能参与比较，否则断线、未预热或计算异常可能误触发执行器。
  return sensorValid && isfinite(value) && isfinite(threshold) && value > threshold;
}

bool isBelowLowerLimit(float value, float threshold) {
  // 两路光敏 ADC 正常情况下始终会得到有限数；这里仍保留检查，防止以后更换
  // 传感器实现后异常值直接触发欠光告警。
  return isfinite(value) && isfinite(threshold) && value < threshold;
}

float normalizedOvershoot(float value, float threshold, float fullLoadSpan,
                          bool sensorValid) {
  // 把“超过上限多少”转换成 0..1。未就绪、无效、未超限全部返回 0；超过
  // fullLoadSpan 后封顶为 1，避免极端数据把后续转速计算推到合法范围之外。
  if (!sensorValid || !isfinite(value) || !isfinite(threshold) ||
      !isfinite(fullLoadSpan) || fullLoadSpan <= 0.0f) {
    return 0.0f;
  }
  const float excess = value - threshold;
  if (excess <= 0.0f) return 0.0f;
  return constrain(excess / fullLoadSpan, 0.0f, 1.0f);
}

float automaticLightLoad(float lightPercent, float holdPercent) {
  // 保持率就是目标光照强度，单位与传感器光照率相同（0..100%）。直接使用
  // 比例误差形成连续目标，不再用开/关状态机，避免 LED 反照造成整路闪烁。
  if (!isfinite(lightPercent) || !isfinite(holdPercent) || holdPercent <= 0.0f) {
    return 0.0f;
  }
  return constrain((holdPercent - lightPercent) / holdPercent, 0.0f, 1.0f);
}

// 综合传感器读数与阈值，产出各项告警布尔和 0..1 归一化负荷。
EnvironmentStatus evaluateEnvironment(const SensorData& data,
                                      const Thresholds& currentThresholds) {
  EnvironmentStatus status;

  // 光照不产生蜂鸣器告警；光敏1表示室内目标差值，光敏2表示室外欠光程度。
  status.light1Alert = false;
  status.light2Alert = false;
  status.lightAlert = false;
  status.light1Load = automaticLightLoad(data.light1Percent,
                                         currentThresholds.lightHoldPercent);
  status.light2Load = automaticLightLoad(
      data.light2Percent, currentThresholds.outdoorLightThresholdPercent);

  // MQ135 预热结束前不产生 AQI 告警或风扇负荷。DHT11 只有本轮温湿度同时
  // 读取成功时 dhtValid 才为 true，避免使用上一次缓存值控制设备。
  status.airAlert = isAboveUpperLimit(data.mq135AqiEstimate,
                                      currentThresholds.mq135Aqi,
                                      data.mq135WarmedUp);
  status.temperatureAlert = isAboveUpperLimit(data.temperatureC,
                                              currentThresholds.temperatureC,
                                              data.dhtValid);
  status.humidityAlert = isAboveUpperLimit(data.humidityPercent,
                                           currentThresholds.humidityPercent,
                                           data.dhtValid);
  status.airLoad = normalizedOvershoot(data.mq135AqiEstimate,
                                       currentThresholds.mq135Aqi,
                                       kAirLoadSpanAqi, data.mq135WarmedUp);
  status.temperatureLoad = normalizedOvershoot(data.temperatureC,
                                               currentThresholds.temperatureC,
                                               kTemperatureLoadSpanC, data.dhtValid);
  status.humidityLoad = normalizedOvershoot(data.humidityPercent,
                                            currentThresholds.humidityPercent,
                                            kHumidityLoadSpanPercent, data.dhtValid);
  return status;
}

// 给自动照明返回一个方向指令：+1 提亮、-1 变暗、0 保持。
int8_t automaticLightDirection(float currentPercent, float targetPercent) {
  // 室内光照误差超过 2 个百分点才调整，目标上下 2% 内保持当前 PWM。
  constexpr float kLightDeadbandPercent = 2.0f;
  if (!isfinite(currentPercent) || !isfinite(targetPercent)) return 0;
  if (targetPercent <= 0.0f) return -1;
  const float error = targetPercent - currentPercent;
  if (error > kLightDeadbandPercent) return 1;
  if (error < -kLightDeadbandPercent) return -1;
  return 0;
}

// 查询 RTC 判断当前是否处于用户设置的自动调光时段内。
bool automaticLightingScheduleActive(const Thresholds& currentThresholds) {
  RtcDateTime now;
  if (!rtcClock.read(now)) return false;
  const uint16_t minuteOfDay = static_cast<uint16_t>(now.hour * 60U + now.minute);
  return lightingScheduleContainsMinute(currentThresholds, minuteOfDay);
}

// 带 1% 迟滞维护“室外是否够暗”的状态，防止阈值附近来回抖动。
bool updateOutdoorDarkState(float outdoorPercent, float thresholdPercent) {
  constexpr float kOutdoorExitHysteresisPercent = 1.0f;
  if (!isfinite(outdoorPercent) || !isfinite(thresholdPercent) ||
      thresholdPercent <= 0.0f) {
    automaticOutdoorDark = false;
    return false;
  }
  if (!automaticOutdoorDark) {
    automaticOutdoorDark = outdoorPercent < thresholdPercent;
  } else {
    const float exitThreshold =
        constrain(thresholdPercent + kOutdoorExitHysteresisPercent, 0.0f, 100.0f);
    if (outdoorPercent >= exitThreshold) automaticOutdoorDark = false;
  }
  return automaticOutdoorDark;
}

// 按用户分别设置的延时维护灯光和风扇的“最近有人”状态。
void updatePresenceRecentState(bool presenceDetected,
                               const Thresholds& currentThresholds) {
  const uint32_t now = millis();
  if (!presenceTrackingStarted) {
    presenceTrackingStarted = true;
    lastPresenceDetectedMs = now;
  }
  if (presenceDetected) lastPresenceDetectedMs = now;
  const uint32_t fanTimeoutMs = currentThresholds.fanNoPresenceTimeoutSeconds * 1000UL;
  const uint32_t lightingTimeoutMs =
      currentThresholds.lightingNoPresenceTimeoutSeconds * 1000UL;
  const uint32_t elapsed = now - lastPresenceDetectedMs;
  automaticFanPresenceRecent = elapsed < fanTimeoutMs;
  automaticPresenceRecent = elapsed < lightingTimeoutMs;
}

// 依据 AQI/温度/湿度负荷合成一个 0..100% 的自动转速目标。
uint8_t automaticFanSpeed(const EnvironmentStatus& status) {
  // 取 AQI、温度、湿度中最大的负荷作为主负荷，保证单项严重超限时风扇能
  // 快速提速；其余两项按 50% 权重叠加，使多项轻微超限也能适当增加转速。
  const float loads[3] = {status.airLoad, status.temperatureLoad,
                          status.humidityLoad};
  float dominant = 0.0f;
  float total = 0.0f;
  for (const float load : loads) {
    if (load > dominant) dominant = load;
    total += load;
  }
  if (dominant <= 0.0f) return 0;

  const float secondary = total - dominant;
  const float blendedLoad = constrain(dominant + secondary * 0.5f, 0.0f, 1.0f);
  const float adjustableRange = static_cast<float>(kFanMaximumSpeedPercent -
                                                   FanHardware::kMinimumRunningSpeedPercent);
  return static_cast<uint8_t>(FanHardware::kMinimumRunningSpeedPercent +
                              blendedLoad * adjustableRange + 0.5f);
}

// 决定通风舵机开合角度：多项告警才打开。
uint16_t automaticServoAngle(const EnvironmentStatus& status) {
  // 光照不足不参与开窗舵机判断。只有空气质量、温度、湿度三项计数，至少
  // 两项同时告警才打开，避免单个传感器瞬时波动频繁推动舵机。
  const uint8_t exceededCount = (status.airAlert ? 1U : 0U) +
                                (status.temperatureAlert ? 1U : 0U) +
                                (status.humidityAlert ? 1U : 0U);
  return exceededCount >= 2 ? kServoOpenAngle : kServoClosedAngle;
}

// 综合最新传感器与环境状态，把自动目标下发给 LED / 风扇 / 舵机。
void applyAutomaticControl(const EnvironmentStatus& status, const SensorData& data,
                           const Thresholds& currentThresholds,
                           bool lightSampled) {
  updatePresenceRecentState(data.presenceDetected, currentThresholds);
  const bool lightingPresenceRecentState = automaticPresenceRecent;
  const bool fanPresenceRecentState = automaticFanPresenceRecent;
  // 每个新光照样本只生成一次“提高、降低、保持”命令，避免对旧数据重复推进 PWM。
  if (lightSampled) {
    if (!automaticLightingScheduleActive(currentThresholds) || !lightingPresenceRecentState) {
      automaticOutdoorDark = false;
      lightingTurnOffAutomatic();
    } else {
      const bool outdoorIsDark = updateOutdoorDarkState(
          data.light2Percent, currentThresholds.outdoorLightThresholdPercent);
      if (!outdoorIsDark) {
        lightingTurnOffAutomatic();
      } else {
        const int8_t direction = automaticLightDirection(
            data.light1Percent, currentThresholds.lightHoldPercent);
        lightingAdjustAutomatic(direction);
      }
    }
  }

  // 自动风扇固定使用正转；达到用户设置的无人延时后目标直接归零。手动模式会忽略
  // 此处目标，重新检测到人后再按温湿度和 AQI 负荷恢复自动转速。
  fanApplyAutomatic(1, fanPresenceRecentState ? automaticFanSpeed(status) : 0);

  // 下方舵机输出函数同样只执行最终角度，并在 WebUI 手动模式下忽略自动目标。
  servoApplyAutomaticAngle(automaticServoAngle(status));
}

}  // namespace

// ===========================================================================
// 传感器初始化与读取实现
// ===========================================================================
// SensorData 和 SensorManager 的声明仍放在 sensors.h，方便 OLED、WebUI 和上报
// 模块共享数据类型；所有会接触 GPIO、ADC、DHT11 的实现都集中在本文件。

// 配置 ADC/DHT11/LD2402 引脚，记录 MQ135 预热起点。
void SensorManager::begin() {
  // ESP32-S3 ADC 配置为 12 位，原始范围为 0..4095。11dB 衰减允许测量较宽
  // 电压范围，但 GPIO 实际输入仍绝不能超过 3.3V，MQ135 必须先经过分压。
  analogReadResolution(12);
  analogSetPinAttenuation(Pins::kLight1, ADC_11db);
  analogSetPinAttenuation(Pins::kLight2, ADC_11db);
  analogSetPinAttenuation(Pins::kMq135, ADC_11db);
  pinMode(Pins::kLight1, INPUT);
  pinMode(Pins::kLight2, INPUT);
  pinMode(Pins::kMq135, INPUT);
  pinMode(Pins::kLd2402, INPUT_PULLDOWN);

  // DHT 库内部负责单总线时序。MQ135 在 begin 时只记录预热起点，不能立即
  // 把冷机读数当成空气质量，否则加热器尚未稳定会产生严重误判。
  dht_.begin();
  mq135StartedMs_ = millis();
  mq135BaselineSum_ = 0;
  mq135BaselineSamples_ = 0;
}

// 一轮采集：光敏 100ms、环境组 2s、返回本轮是否更新了光敏。
bool SensorManager::update() {
  const uint32_t now = millis();

  // LD2402 是数字量且不受 DHT11 最短采样周期限制，因此每轮主循环都读取，
  // 让 WebUI 的人体存在状态能快速响应。
  data_.presenceDetected = digitalRead(Pins::kLd2402) == HIGH;

  // 两路光敏每 100ms 独立采样一次，为 LED 增量闭环提供及时反馈。
  bool lightSampled = false;
  if (!lightSampledOnce_ || now - lastLightSampleMs_ >= kLightSampleIntervalMs) {
    lightSampledOnce_ = true;
    lastLightSampleMs_ = now;
    data_.light1Raw = readAveragedAdc(Pins::kLight1);
    data_.light2Raw = readAveragedAdc(Pins::kLight2);
    data_.light1Percent = SensorHardware::lightPercentFromRaw(
        data_.light1Raw, SensorHardware::kLight1DarkIsHigh);
    data_.light2Percent = SensorHardware::lightPercentFromRaw(
        data_.light2Raw, SensorHardware::kLight2DarkIsHigh);
    lightSampled = true;
  }

  // DHT11 不适合高频读取，MQ135 和温湿度继续每 2 秒更新一次。
  if (sampledOnce_ && now - lastSampleMs_ < kEnvironmentSampleIntervalMs) {
    return lightSampled;
  }
  sampledOnce_ = true;
  lastSampleMs_ = now;

  // 保存 MQ135 原始 ADC 和校准毫伏值，供相对 AQI 与 WebUI 诊断使用。
  data_.mq135Raw = readAveragedAdc(Pins::kMq135);
  data_.mq135MilliVolts = readAveragedMillivolts(Pins::kMq135);

  // MQ135 预热共 3 分钟，只在最后 60 秒累计空气基线。这样可避开冷启动变化
  // 最大的阶段，又能用多个样本平均降低单次 ADC 噪声。
  const uint32_t mqElapsedMs = now - mq135StartedMs_;
  if (mqElapsedMs >= kMq135WarmupMs - kMq135BaselineWindowMs &&
      mqElapsedMs < kMq135WarmupMs) {
    mq135BaselineSum_ += data_.mq135Raw;
    ++mq135BaselineSamples_;
  }

  const bool wasWarmedUp = data_.mq135WarmedUp;
  data_.mq135WarmedUp = mqElapsedMs >= kMq135WarmupMs;
  if (data_.mq135WarmedUp && !wasWarmedUp) {
    // 极端情况下预热窗口没有取得样本，就使用当前值作为兜底基线，避免除以 0。
    data_.mq135BaselineRaw = mq135BaselineSamples_ == 0
                                 ? data_.mq135Raw
                                 : static_cast<uint16_t>(mq135BaselineSum_ /
                                                         mq135BaselineSamples_);
  }
  if (data_.mq135WarmedUp) {
    data_.mq135AqiEstimate = SensorHardware::relativeAqiFromRaw(
        data_.mq135Raw, data_.mq135BaselineRaw);
  }

  // DHT11 温度和湿度来自同一次业务采样，只有两项都为有效数时才更新缓存并
  // 设置 dhtValid。读取失败时保留上次值便于诊断，但 main.cpp 看到 dhtValid
  // 为 false 后不会用旧值告警、驱动风扇或舵机。
  const float temperature = dht_.readTemperature();
  const float humidity = dht_.readHumidity();
  data_.dhtValid = isfinite(temperature) && isfinite(humidity);
  if (data_.dhtValid) {
    data_.temperatureC = temperature;
    data_.humidityPercent = humidity;
  }
  return lightSampled;
}

const SensorData& SensorManager::data() const {
  // 返回引用而不是复制整个结构体；调用方只能读取，不能绕过采集流程修改数据。
  return data_;
}

// 对指定 ADC 引脚做 kAdcSamples 次平均，抑制电源纹波和瞬时噪声。
uint16_t SensorManager::readAveragedAdc(uint8_t pin) {
  uint32_t sum = 0;
  // 8 个 12 位样本最大总和为 32760，使用 uint32_t 可明确避免累计溢出。
  for (uint8_t i = 0; i < SensorHardware::kAdcSamples; ++i) {
    sum += analogRead(pin);
  }
  return static_cast<uint16_t>(sum / SensorHardware::kAdcSamples);
}

// 与 readAveragedAdc 同理，但走 ESP32 校准过的毫伏读数。
uint16_t SensorManager::readAveragedMillivolts(uint8_t pin) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < SensorHardware::kAdcSamples; ++i) {
    sum += analogReadMilliVolts(pin);
  }
  return static_cast<uint16_t>(sum / SensorHardware::kAdcSamples);
}

// ===========================================================================
// 三路 LED 初始化、自动输出和 WebUI 手动覆盖
// ===========================================================================

void lightingInit() {
  // CJ3400 是低侧 NMOS，Gate 为高时灯亮。先把输出锁存器设为 LOW，再切换为
  // OUTPUT，可避免启动瞬间 Gate 浮空导致三只灯闪烁。硬件仍应有约 10k 下拉。
  digitalWrite(Pins::kLed1, LOW);
  digitalWrite(Pins::kLed2, LOW);
  digitalWrite(Pins::kLed3, LOW);
  pinMode(Pins::kLed1, OUTPUT);
  pinMode(Pins::kLed2, OUTPUT);
  pinMode(Pins::kLed3, OUTPUT);

  // 每只灯绑定独立 LEDC channel/timer，所以 WebUI 手动模式可以设置不同亮度，
  // 且三路 PWM 的定时器配置互不覆盖。
  const uint32_t led1Frequency = ledcSetup(
      LightingHardware::kLed1Channel, LightingHardware::kPwmFrequencyHz,
      LightingHardware::kPwmResolutionBits);
  const uint32_t led2Frequency = ledcSetup(
      LightingHardware::kLed2Channel, LightingHardware::kPwmFrequencyHz,
      LightingHardware::kPwmResolutionBits);
  const uint32_t led3Frequency = ledcSetup(
      LightingHardware::kLed3Channel, LightingHardware::kPwmFrequencyHz,
      LightingHardware::kPwmResolutionBits);
  if (led1Frequency == 0 || led2Frequency == 0 || led3Frequency == 0) {
    Serial.printf("[LIGHT] ERROR: LEDC init failed frequencies=%lu/%lu/%luHz\r\n",
                  static_cast<unsigned long>(led1Frequency),
                  static_cast<unsigned long>(led2Frequency),
                  static_cast<unsigned long>(led3Frequency));
    return;
  }

  ledcAttachPin(Pins::kLed1, LightingHardware::kLed1Channel);
  ledcAttachPin(Pins::kLed2, LightingHardware::kLed2Channel);
  ledcAttachPin(Pins::kLed3, LightingHardware::kLed3Channel);
  ledcWrite(LightingHardware::kLed1Channel, 0);
  ledcWrite(LightingHardware::kLed2Channel, 0);
  ledcWrite(LightingHardware::kLed3Channel, 0);
  LightingHardware::ready = true;
  Serial.printf("[LIGHT] initialized LED1=GPIO%u LED2=GPIO%u LED3=GPIO%u pwm=%luHz resolution=%ubit maxDuty=%u/%u maxAverage=%umV@%umV\r\n",
                Pins::kLed1, Pins::kLed2, Pins::kLed3,
                static_cast<unsigned long>(LightingHardware::kPwmFrequencyHz),
                LightingHardware::kPwmResolutionBits,
                LightingHardware::kPwmMaxDuty,
                LightingHardware::kPwmFullScaleDuty,
                LightingHardware::kMaximumAverageMillivolts,
                LightingHardware::kLedSupplyMillivolts);
}

// 自动模式下按 direction 增/减/保持三路同步占空比。
void lightingAdjustAutomatic(int8_t direction) {
  // 手动模式下忽略自动增量命令，才能让 WebUI 滑块保持稳定。
  if (!LightingHardware::ready || LightingHardware::manual) return;
  if (direction < -1 || direction > 1) return;

  const uint16_t synchronizedDuty = LightingHardware::adjustedAutomaticDuty(
      direction, LightingHardware::led2Duty);
  LightingHardware::writeDuty(
      LightingHardware::kLed1Channel,
      synchronizedDuty, LightingHardware::led1Duty);
  LightingHardware::writeDuty(
      LightingHardware::kLed2Channel,
      synchronizedDuty, LightingHardware::led2Duty);
  LightingHardware::writeDuty(
      LightingHardware::kLed3Channel,
      synchronizedDuty, LightingHardware::led3Duty);

  const uint32_t now = millis();
  if (now - LightingHardware::lastLogMs >= 2000) {
    Serial.printf("[LIGHT] automatic dir=%d synchronizedDuty=%u\r\n",
                  direction, synchronizedDuty);
    LightingHardware::lastLogMs = now;
  }
}

// 自动模式下把三路 LED 全部关掉；手动模式不受影响。
void lightingTurnOffAutomatic() {
  if (!LightingHardware::ready || LightingHardware::manual) return;
  LightingHardware::writeDuty(
      LightingHardware::kLed1Channel, 0, LightingHardware::led1Duty);
  LightingHardware::writeDuty(
      LightingHardware::kLed2Channel, 0, LightingHardware::led2Duty);
  LightingHardware::writeDuty(
      LightingHardware::kLed3Channel, 0, LightingHardware::led3Duty);
}

// WebUI 手动模式：三路各自设置 0..100% 亮度，进入 manual 状态。
bool lightingSetManual(uint8_t led1Percent, uint8_t led2Percent,
                       uint8_t led3Percent) {
  if (!LightingHardware::ready || led1Percent > 100 || led2Percent > 100 ||
      led3Percent > 100) {
    return false;
  }
  LightingHardware::manual = true;
  LightingHardware::writeDuty(
      LightingHardware::kLed1Channel,
      LightingHardware::dutyFromPercent(led1Percent), LightingHardware::led1Duty);
  LightingHardware::writeDuty(
      LightingHardware::kLed2Channel,
      LightingHardware::dutyFromPercent(led2Percent), LightingHardware::led2Duty);
  LightingHardware::writeDuty(
      LightingHardware::kLed3Channel,
      LightingHardware::dutyFromPercent(led3Percent), LightingHardware::led3Duty);
  Serial.printf("[LIGHT] manual LED1=%u%%(duty=%u) LED2=%u%%(duty=%u) LED3=%u%%(duty=%u) maxAverage=%umV\r\n",
                led1Percent, LightingHardware::led1Duty,
                led2Percent, LightingHardware::led2Duty,
                led3Percent, LightingHardware::led3Duty,
                LightingHardware::kMaximumAverageMillivolts);
  return true;
}

// 退出手动模式；下一轮 loop() 会根据光敏读数重新自动调光。
void lightingResumeAutomatic() {
  // 此处只退出手动覆盖。loop() 下一轮会使用最新光敏读数重新计算并下发亮度。
  LightingHardware::manual = false;
  Serial.println("[LIGHT] automatic light-sensor control resumed");
}

bool lightingReady() { return LightingHardware::ready; }

bool lightingManualMode() { return LightingHardware::manual; }

bool lightingOutdoorDark() { return automaticOutdoorDark; }

bool lightingPresenceRecent() { return automaticPresenceRecent; }

uint8_t lightingBrightnessPercent(uint8_t ledIndex) {
  uint16_t duty = 0;
  if (ledIndex == 1) duty = LightingHardware::led1Duty;
  else if (ledIndex == 2) duty = LightingHardware::led2Duty;
  else if (ledIndex == 3) duty = LightingHardware::led3Duty;
  else return 0;
  return static_cast<uint8_t>(
      (static_cast<uint32_t>(duty) * 100U + LightingHardware::kPwmMaxDuty / 2U) /
      LightingHardware::kPwmMaxDuty);
}

// ===========================================================================
// L9110H 风扇初始化、自动输出和 WebUI 手动覆盖
// ===========================================================================

void fanInit() {
  // MCPWM0A/0B 是“当前 MCPWM 单元内”的发生器名称。这里的单元是 UNIT_1，
  // 所以不会与下面舵机使用的 UNIT_0/MCPWM0A 冲突。
  const esp_err_t gpioA = mcpwm_gpio_init(
      FanHardware::kUnit, MCPWM0A, Pins::kFanIn1);
  const esp_err_t gpioB = mcpwm_gpio_init(
      FanHardware::kUnit, MCPWM0B, Pins::kFanIn2);
  mcpwm_config_t config{};
  config.frequency = FanHardware::kPwmFrequencyHz;
  config.cmpr_a = 0;
  config.cmpr_b = 0;
  config.duty_mode = MCPWM_DUTY_MODE_0;
  config.counter_mode = MCPWM_UP_COUNTER;
  const esp_err_t initResult =
      (gpioA == ESP_OK && gpioB == ESP_OK)
          ? mcpwm_init(FanHardware::kUnit, FanHardware::kTimer, &config)
          : (gpioA != ESP_OK ? gpioA : gpioB);
  if (initResult != ESP_OK) {
    Serial.printf("[FAN] ERROR: L9110H MCPWM init failed IN1=%u IN2=%u err=%d\r\n",
                  Pins::kFanIn1, Pins::kFanIn2, static_cast<int>(initResult));
    return;
  }
  FanHardware::ready = true;
  FanHardware::applyPwm(1, 0);
  Serial.printf("[FAN] initialized L9110H IN1=%u IN2=%u pwm=%luHz speed=0..100%%\r\n",
                Pins::kFanIn1, Pins::kFanIn2,
                static_cast<unsigned long>(FanHardware::kPwmFrequencyHz));
}

// 自动风扇每 100ms 向目标 ±1% 靠拢；起停/换向单独走可靠起转速度。
void fanApplyAutomatic(int8_t nextDirection, uint8_t desiredSpeed) {
  if (!FanHardware::ready || FanHardware::manual) return;
  if ((nextDirection != -1 && nextDirection != 1) || desiredSpeed > 100) return;
  if (desiredSpeed != 0 && desiredSpeed < FanHardware::kMinimumRunningSpeedPercent) {
    desiredSpeed = FanHardware::kMinimumRunningSpeedPercent;
  }

  const uint32_t now = millis();
  if (now - FanHardware::lastLogMs >= 2000) {
    Serial.printf("[FAN] automatic target direction=%d speed=%u%% current=%d/%u%%\r\n",
                  nextDirection, desiredSpeed, FanHardware::direction,
                  FanHardware::speed);
    FanHardware::lastLogMs = now;
  }

  // 停机立即执行；启动和换向先进入 40% 可靠起转区，避免低占空比堵转。
  if (desiredSpeed == 0) {
    if (FanHardware::speed != 0 || FanHardware::automaticNeedsApply) {
      FanHardware::applyPwm(nextDirection, 0);
    }
    FanHardware::automaticNeedsApply = false;
    return;
  }
  if (FanHardware::speed < FanHardware::kMinimumRunningSpeedPercent ||
      nextDirection != FanHardware::direction) {
    FanHardware::applyPwm(nextDirection, FanHardware::kMinimumRunningSpeedPercent);
    FanHardware::lastAutomaticStepMs = now;
    FanHardware::automaticNeedsApply = false;
    return;
  }

  const uint8_t delta = desiredSpeed > FanHardware::speed
                            ? desiredSpeed - FanHardware::speed
                            : FanHardware::speed - desiredSpeed;
  if (delta <= FanHardware::kSpeedTolerancePercent) {
    FanHardware::automaticNeedsApply = false;
    return;
  }
  if (!FanHardware::automaticNeedsApply &&
      now - FanHardware::lastAutomaticStepMs < FanHardware::kAutomaticStepIntervalMs) {
    return;
  }

  const uint8_t nextSpeed = desiredSpeed > FanHardware::speed
                                ? FanHardware::speed + FanHardware::kSpeedStepPercent
                                : FanHardware::speed - FanHardware::kSpeedStepPercent;
  FanHardware::applyPwm(nextDirection, nextSpeed);
  FanHardware::lastAutomaticStepMs = now;
  FanHardware::automaticNeedsApply = false;
}

// WebUI 手动模式：直接写入方向和转速，进入 manual 状态。
bool fanSetManual(int8_t nextDirection, uint8_t speedPercent) {
  if (!FanHardware::ready || nextDirection == 0 || speedPercent > 100) return false;
  FanHardware::manual = true;
  FanHardware::applyPwm(nextDirection, speedPercent);
  Serial.printf("[FAN] manual direction=%d speed=%u%%\r\n",
                nextDirection, speedPercent);
  return true;
}

// 退出手动模式，让下一轮 loop() 根据环境负荷重新决定转速。
void fanResumeAutomatic() {
  // 不在按钮回调里猜测自动转速，交给下一轮 loop() 使用最新传感器状态决定。
  FanHardware::manual = false;
  FanHardware::automaticNeedsApply = true;
  Serial.println("[FAN] automatic threshold control resumed");
}

bool fanReady() { return FanHardware::ready; }
bool fanManualMode() { return FanHardware::manual; }
bool fanRunning() { return FanHardware::running; }
int8_t fanDirection() { return FanHardware::direction; }
uint8_t fanSpeedPercent() { return FanHardware::speed; }

// ===========================================================================
// SG90 舵机初始化、自动输出和 WebUI 手动覆盖
// ===========================================================================

// 初始化 SG90 舵机 MCPWM（UNIT_0, 50Hz, 500..2500us 脉宽）。
void servoInit() {
  pinMode(Pins::kServo, OUTPUT);
  const esp_err_t gpioResult = mcpwm_gpio_init(
      ServoHardware::kUnit, MCPWM0A, Pins::kServo);
  mcpwm_config_t config{};
  config.frequency = ServoHardware::kPwmFrequencyHz;
  config.cmpr_a = 7.5f;
  config.cmpr_b = 0.0f;
  config.duty_mode = MCPWM_DUTY_MODE_0;
  config.counter_mode = MCPWM_UP_COUNTER;
  const esp_err_t initResult = gpioResult == ESP_OK
                                   ? mcpwm_init(ServoHardware::kUnit,
                                                ServoHardware::kTimer, &config)
                                   : gpioResult;
  if (initResult != ESP_OK) {
    Serial.printf("[SERVO] ERROR: MCPWM init failed gpio=%u err=%d\r\n",
                  Pins::kServo, static_cast<int>(initResult));
    return;
  }
  ServoHardware::ready = true;
  ServoHardware::writeAngle(0);
  Serial.printf("[SERVO] initialized gpio=%u MCPWM=0A pwm=%uHz pulse=500..2500us range=0..180deg reversed=%s\r\n",
                Pins::kServo,
                static_cast<unsigned int>(ServoHardware::kPwmFrequencyHz),
                ServoHardware::kReverseDirection ? "yes" : "no");
}

// 自动模式下把舵机转到指定角度；同角度不重复写寄存器。
void servoApplyAutomaticAngle(uint16_t angle) {
  if (!ServoHardware::ready || ServoHardware::manual || angle > 180) return;
  // 目标不变时不重复输出。用户从手动恢复自动后 automaticNeedsApply 为 true，
  // 即使角度数值碰巧相同，也会明确执行一次最新自动目标。
  if (!ServoHardware::automaticNeedsApply &&
      angle == ServoHardware::currentAngle) {
    return;
  }
  ServoHardware::writeAngle(angle);
  ServoHardware::automaticNeedsApply = false;
  Serial.printf("[SERVO] automatic target=%u deg\r\n", angle);
}

// WebUI 手动模式：直接锁定舵机角度，进入 manual 状态。
bool servoSetManualAngle(uint16_t angle) {
  if (!ServoHardware::ready || angle > 180) return false;
  ServoHardware::manual = true;
  ServoHardware::writeAngle(angle);
  Serial.printf("[SERVO] manual target=%u deg\r\n", angle);
  return true;
}

// 退出手动模式，下一轮 loop() 会重新计算并下发自动角度。
void servoResumeAutomatic() {
  ServoHardware::manual = false;
  ServoHardware::automaticNeedsApply = true;
  Serial.println("[SERVO] automatic threshold control resumed");
}

bool servoReady() { return ServoHardware::ready; }
bool servoManualMode() { return ServoHardware::manual; }
uint16_t servoCurrentAngle() { return ServoHardware::currentAngle; }

void setup() {
  // ----------------------------- 串口和启动信息 -----------------------------
  Serial.begin(115200);
  // 项目使用开发板 USB-UART 桥接串口；与 ROM 启动日志保持同一端口。
  // 短暂等待不会阻塞 UART，可给串口监视器留出复位后的连接时间。
  const uint32_t serialReadyDeadline = millis() + 1500;
  while (!Serial && millis() < serialReadyDeadline) delay(10);
  Serial.setDebugOutput(true);
  esp_log_level_set("*", ESP_LOG_INFO);
  delay(100);
  Serial.println("\n[boot] Classroom ESP32 monitor");
  Serial.printf("[MEM] flash=%uMB psram=%s psram_size=%uMB\r\n",
                static_cast<unsigned int>(ESP.getFlashChipSize() / (1024 * 1024)),
                psramFound() ? "yes" : "no",
                static_cast<unsigned int>(ESP.getPsramSize() / (1024 * 1024)));
  // 低电平触发蜂鸣器必须尽早拉高，避免其在 Wi-Fi/OLED 初始化期间误响。
  buzzerInit();

  // ----------------------------- 配置和显示设备 -----------------------------
  // 先从 NVS 恢复阈值，再初始化 OLED。这样第一张正式监测画面使用的就是上次
  // 保存的阈值，而不是短暂显示默认值后再跳变。
  thresholdStoreBegin(thresholds);
  OledController::begin();

  // RTC 使用独立 I2C 总线。即使 RTC 不存在，内部错误状态也不会阻止后续
  // PWM、网络和传感器初始化。
  rtcClock.begin();

  // ------------------------------ 三类执行器 -------------------------------
  // 舵机、风扇和 LED 的完整初始化实现在本文件上方。先让执行器进入安全初始
  // 状态，再启动网络，避免 Wi-Fi 连接过程较长时 GPIO 保持不确定电平。
  servoInit();
  fanInit();
  lightingInit();

  // --------------------------- 网络、输入和传感器 ---------------------------
  // 网络模块负责 Wi-Fi、WebUI 和数据上报；编码器模块负责本地旋钮。传感器
  // begin() 最后配置 ADC、LD2402 和 DHT11，并开始 MQ135 的三分钟预热计时。
  networkPortal.setThresholds(thresholds);
  networkPortal.begin();
  controlInit();
  sensors.begin();

  // 给 OLED 和外设短暂稳定时间。之后 Arduino 框架会不断调用 loop()。
  delay(800);
}

void loop() {
  // 第一步：传感器读取函数只完成采集，network_portal.cpp 只维护网络通信，
  // 二者都不在这一阶段判断阈值或决定执行器目标。
  const bool lightSampled = sensors.update();
  controlUpdate();
  networkPortal.update(sensors.data());

  // 第二步：main.cpp 使用同一份传感器快照计算环境状态。OLED、蜂鸣器和三个
  // 自动执行器随后共享这份结果，保证所有模块对“是否超限”的理解一致。
  const EnvironmentStatus environmentStatus = evaluateEnvironment(sensors.data(), thresholds);

  // 第三步：把五个独立告警编码成 bit mask。buzzer.cpp 只负责启用状态、
  // 单次鸣叫时序和 NVS，不再判断任何传感器数值。
  const uint8_t environmentAlertMask =
      (environmentStatus.airAlert ? 0x04 : 0x00) |
      (environmentStatus.temperatureAlert ? 0x08 : 0x00) |
      (environmentStatus.humidityAlert ? 0x10 : 0x00);
  buzzerUpdate(environmentAlertMask);

  // 第四步：在 main.cpp 计算 LED 亮度、风扇转速和舵机角度，再把最终目标
  // 下发给各自驱动。驱动内部的手动覆盖会决定是否接受本轮自动目标。
  applyAutomaticControl(environmentStatus, sensors.data(), thresholds, lightSampled);

  // OLED 页面状态、旋钮交互和绘制调度全部由独立显示控制模块负责。
  OledController::update(thresholds, sensors.data(), environmentStatus, networkPortal);

  delay(15);
}
