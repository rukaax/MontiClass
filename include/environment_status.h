/**
 * @file environment_status.h
 * @brief main.cpp 计算后交给显示、告警和执行器使用的环境状态数据。
 */
#pragma once

struct EnvironmentStatus {
  // 告警位只表达“是否越过阈值”，不在本结构体内执行任何比较。
  // 所有传感器有效性检查和阈值判断统一放在 main.cpp，避免多个模块
  // 对同一传感器使用不同规则。
  bool light1Alert = false;
  bool light2Alert = false;
  bool lightAlert = false;
  bool airAlert = false;
  bool temperatureAlert = false;
  bool humidityAlert = false;
  // 归一化负荷范围为 0..1。光照负荷表示“低于下限的程度”，其余负荷
  // 表示“高于上限的程度”。main.cpp 保证无效读数对应的负荷始终为 0。
  float light1Load = 0.0f;
  float light2Load = 0.0f;
  float airLoad = 0.0f;
  float temperatureLoad = 0.0f;
  float humidityLoad = 0.0f;
};
