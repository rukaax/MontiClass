/** @file thresholds.h
 * @brief 环境告警阈值的共享数据模型。
 */
#pragma once

struct Thresholds {
  float lightPercent = 50.0f;
  float mq135Ppm = 500.0f;
  float temperatureC = 30.0f;
  float humidityPercent = 60.0f;
};
