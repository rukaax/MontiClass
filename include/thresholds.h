/** @file thresholds.h
 * @brief 环境告警阈值的共享数据模型。
 */
#pragma once

struct Thresholds {
  float lightPercent = 50.0f;  // 下限；任意一路光照低于该值即告警
  float mq135Aqi = 100.0f;
  float temperatureC = 30.0f;
  float humidityPercent = 60.0f;
};
