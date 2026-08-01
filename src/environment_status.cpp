/** @file environment_status.cpp */
#include "environment_status.h"

#include <math.h>

namespace {
bool exceeds(float value, float threshold) {
  // 无效读数绝不触发告警，避免 DHT 断线时画出误导性的警示符号。
  return !isnan(value) && value > threshold;
}
}  // namespace

EnvironmentStatus evaluateEnvironment(const SensorData& data, const Thresholds& thresholds) {
  EnvironmentStatus status;
  status.lightAlert = exceeds(data.lightPercent, thresholds.lightPercent);
  status.airAlert = data.mq135WarmedUp && exceeds(data.mq135PpmEstimate, thresholds.mq135Ppm);
  status.temperatureAlert = data.dhtValid && exceeds(data.temperatureC, thresholds.temperatureC);
  status.humidityAlert = data.dhtValid && exceeds(data.humidityPercent, thresholds.humidityPercent);
  return status;
}
