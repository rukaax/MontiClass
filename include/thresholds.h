/** @file thresholds.h
 * @brief 环境告警阈值的共享数据模型。
 */
#pragma once

#include <stdint.h>

struct Thresholds {
  float outdoorLightThresholdPercent = 50.0f;  // 光敏2低于此值才允许自动照明
  float lightHoldPercent = 50.0f;  // 自动照明保持的目标光照率，0..100%
  float mq135Aqi = 100.0f;
  float temperatureC = 30.0f;
  float humidityPercent = 60.0f;
  uint16_t lightingStartMinute = 18U * 60U;  // 自动调光开始时间，按当天分钟计
  uint16_t lightingEndMinute = 6U * 60U;     // 自动调光结束时间，支持跨午夜
};

inline bool lightingScheduleContainsMinute(const Thresholds& thresholds,
                                           uint16_t minuteOfDay) {
  const uint16_t start = thresholds.lightingStartMinute;
  const uint16_t end = thresholds.lightingEndMinute;
  if (start == end) return true;  // 起止相同表示全天允许自动调光
  if (start < end) return minuteOfDay >= start && minuteOfDay < end;
  return minuteOfDay >= start || minuteOfDay < end;
}
