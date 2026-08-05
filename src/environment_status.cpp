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
  return status;
}
