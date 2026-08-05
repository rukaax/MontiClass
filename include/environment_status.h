/** @file environment_status.h
 * @brief 传感器读数与阈值比较后的状态。
 */
#pragma once

#include "sensors.h"
#include "thresholds.h"

struct EnvironmentStatus {
  // 两路光敏共用下限阈值；任意一路低于阈值即提示。
  bool light1Alert = false;
  bool light2Alert = false;
  bool lightAlert = false;
  bool airAlert = false;
  bool temperatureAlert = false;
  bool humidityAlert = false;
};

// 集中处理所有超限规则；显示模块只使用结果，不拥有业务判断。
EnvironmentStatus evaluateEnvironment(const SensorData& data, const Thresholds& thresholds);
