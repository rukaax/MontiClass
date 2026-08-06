/** @file environment_status.cpp */
#include "environment_status.h"

#include <math.h>

namespace {
bool exceeds(float value, float threshold) {
  // 无效读数绝不触发告警，避免 DHT 断线时画出误导性的警示符号。
  return !isnan(value) && value > threshold;
}

bool fallsBelow(float value, float threshold) {
  return !isnan(value) && value < threshold;
}

// 超出阈值多少才算“满负荷”。这些跨度决定风扇的调速曲线陡缓，
// 与阈值一起构成唯一的环境判断来源，执行器模块不再自己猜测量纲。
constexpr float kAirLoadSpanAqi = 100.0f;
constexpr float kTemperatureLoadSpanC = 6.0f;
constexpr float kHumidityLoadSpanPercent = 20.0f;

// 把“超出阈值的绝对量”折算成 0..1 的负荷，供调速使用。
float overshootLoad(float value, float threshold, float span, bool valid) {
  if (!valid || isnan(value) || span <= 0.0f) return 0.0f;
  const float excess = value - threshold;
  if (excess <= 0.0f) return 0.0f;
  return excess >= span ? 1.0f : excess / span;
}
}  // namespace

EnvironmentStatus evaluateEnvironment(const SensorData& data, const Thresholds& thresholds) {
  EnvironmentStatus status;
  // 光照阈值是下限：任意一路低于它即认为照度不足。
  status.light1Alert = fallsBelow(data.light1Percent, thresholds.lightPercent);
  status.light2Alert = fallsBelow(data.light2Percent, thresholds.lightPercent);
  status.lightAlert = status.light1Alert || status.light2Alert;
  status.airAlert = data.mq135WarmedUp &&
                    exceeds(data.mq135AqiEstimate, thresholds.mq135Aqi);
  status.temperatureAlert = data.dhtValid && exceeds(data.temperatureC, thresholds.temperatureC);
  status.humidityAlert = data.dhtValid && exceeds(data.humidityPercent, thresholds.humidityPercent);
  status.airLoad = overshootLoad(data.mq135AqiEstimate, thresholds.mq135Aqi, kAirLoadSpanAqi,
                                 data.mq135WarmedUp);
  status.temperatureLoad = overshootLoad(data.temperatureC, thresholds.temperatureC,
                                         kTemperatureLoadSpanC, data.dhtValid);
  status.humidityLoad = overshootLoad(data.humidityPercent, thresholds.humidityPercent,
                                      kHumidityLoadSpanPercent, data.dhtValid);
  return status;
}
