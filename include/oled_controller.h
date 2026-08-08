/** @file oled_controller.h - OLED 页面状态与交互控制接口。 */
#pragma once  // 防止本头文件在同一编译单元中被重复包含。

#include "environment_status.h"  // 提供 OLED 监测页需要的环境判断结果。
#include "sensors.h"             // 提供 OLED 页面需要的实时传感器数据。
#include "thresholds.h"          // 提供 OLED 设置页读写的配置数据。

class NetworkPortal;  // 前置声明网络门户类型，避免在头文件中引入完整网络实现。

namespace OledController {  // 把 OLED 页面控制接口集中在独立命名空间中。

void begin();  // 初始化 OLED 硬件并绘制启动画面。

void update(Thresholds& thresholds, const SensorData& data,  // 接收可编辑设置和实时数据。
            const EnvironmentStatus& status,                 // 接收已经计算好的环境状态。
            NetworkPortal& networkPortal);                    // 接收网络页面状态和网络信息。

}  // namespace OledController  // 结束 OLED 页面控制接口命名空间。
