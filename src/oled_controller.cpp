/** @file oled_controller.cpp - OLED 页面状态、旋钮交互和渲染调度实现。 */
#include "oled_controller.h"  // 引入本模块公开接口和共享数据类型。

#include <Arduino.h>  // 提供 String、constrain 和基础整数类型。

#include "SSD1306.h"          // 提供 SSD1306 初始化及具体页面绘制函数。
#include "control.h"          // 提供 EC11 旋转、短按和长按事件。
#include "network_portal.h"   // 提供联网状态、IP 地址和上报状态。
#include "threshold_store.h"  // 提供设置保存到 NVS 的接口。

namespace OledController {  // 所有 OLED 页面控制实现都归属本模块命名空间。
namespace {                 // 下列页面状态只允许本源文件访问。

enum class Screen : uint8_t { Monitor, Settings };  // 定义监测页和设置页两种主页面。

constexpr uint8_t kFirstSettingIndex = 0;  // 定义设置列表第一个项目的索引。
constexpr uint8_t kLastSettingIndex = 4;   // 定义设置列表最后一个项目的索引。

Screen screen = Screen::Monitor;           // 上电后默认显示实时监测页面。
uint8_t selectedSetting = 0;                // 保存设置页面当前选中的项目。
bool editingSetting = false;                // 表示旋钮当前是否正在修改选中值。
bool settingsDirty = false;                 // 表示内存中的设置是否尚未写入 NVS。
bool networkInfoVisible = false;            // 表示设置页当前是否切到网络信息页面。

void resetToMonitor() {                     // 统一恢复监测页及其相关页面状态。
  screen = Screen::Monitor;                 // 把当前主页面切换为实时监测页。
  editingSetting = false;                   // 退出可能仍在进行的数值编辑状态。
  networkInfoVisible = false;               // 关闭只读网络信息页面。
}                                           // 结束恢复监测页函数。

void adjustSelectedSetting(Thresholds& thresholds, int direction) {  // 按旋钮方向修改当前设置。
  switch (selectedSetting) {                                      // 根据当前项目选择对应配置字段。
    case 0:                                                        // 项目 0 对应室外光照阈值。
      thresholds.outdoorLightThresholdPercent = constrain(        // 把室外阈值限制在合法范围内。
          thresholds.outdoorLightThresholdPercent + direction,    // 每个旋钮刻度调整一个百分点。
          0.0f, 100.0f);                                          // 限定结果只能位于 0 到 100。
      break;                                                       // 完成室外阈值调整后退出分支。
    case 1:                                                        // 项目 1 对应室内光照目标。
      thresholds.lightHoldPercent = constrain(                     // 把保持率限制在合法范围内。
          thresholds.lightHoldPercent + direction, 0.0f, 100.0f); // 每个旋钮刻度调整一个百分点。
      break;                                                       // 完成光照保持率调整后退出分支。
    case 2:                                                        // 项目 2 对应空气 AQI 上限。
      thresholds.mq135Aqi = constrain(                             // 把 AQI 设置限制在合法范围内。
          thresholds.mq135Aqi + direction * 5.0f, 0.0f, 500.0f);  // 每个刻度调整 5 AQI。
      break;                                                       // 完成 AQI 调整后退出分支。
    case 3:                                                        // 项目 3 对应温度上限。
      thresholds.temperatureC = constrain(                         // 把温度设置限制在合法范围内。
          thresholds.temperatureC + direction * 0.5f, 0.0f, 60.0f); // 每个刻度调整 0.5 摄氏度。
      break;                                                       // 完成温度调整后退出分支。
    case 4:                                                        // 项目 4 对应湿度上限。
      thresholds.humidityPercent = constrain(                      // 把湿度设置限制在合法范围内。
          thresholds.humidityPercent + direction, 0.0f, 100.0f);  // 每个刻度调整一个百分点。
      break;                                                       // 完成湿度调整后退出分支。
    default:                                                       // 防御超出设置列表范围的索引。
      return;                                                      // 非法索引不修改数据也不设置脏标志。
  }                                                               // 结束设置项目选择。
  settingsDirty = true;                                           // 标记配置需要在确认时写入 NVS。
}                                                                 // 结束当前设置调整函数。

void updateSettings(Thresholds& thresholds, int direction) {  // 处理设置页的一轮旋钮和按键输入。
  if (editingSetting) {                                      // 编辑模式下旋转应修改数值而非移动光标。
    if (direction != 0) {                                    // 只有实际发生旋转时才修改配置。
      adjustSelectedSetting(thresholds, direction);           // 按旋转方向调整当前选中配置。
    }                                                         // 结束旋转事件判断。
    if (controlWasShortPress()) {                             // 短按表示确认当前修改。
      if (settingsDirty && thresholdStoreSave(thresholds)) {  // 有改动且保存成功时清除脏标志。
        settingsDirty = false;                                // 记录当前配置已持久化。
      }                                                       // 结束 NVS 保存结果处理。
      editingSetting = false;                                 // 无论是否改动都退出编辑模式。
    }                                                         // 结束短按确认处理。
    return;                                                   // 编辑模式本轮不再处理页面导航。
  }                                                           // 结束编辑模式分支。

  if (direction > 0) {                                       // 向上旋转用于返回或移动到前一项。
    if (networkInfoVisible) {                                // 网络信息页向上旋转应先返回设置列表。
      networkInfoVisible = false;                            // 关闭网络信息页并恢复设置列表。
    } else if (selectedSetting > kFirstSettingIndex) {       // 未到第一项时可以继续向上移动。
      --selectedSetting;                                     // 把选择光标移动到前一个项目。
    }                                                        // 结束向上旋转处理。
  } else if (direction < 0) {                                // 向下旋转用于移动到后一项或网络页。
    if (selectedSetting < kLastSettingIndex) {               // 未到最后一项时继续向下移动。
      ++selectedSetting;                                     // 把选择光标移动到后一个项目。
    } else {                                                 // 最后一项继续向下会进入只读网络页。
      networkInfoVisible = true;                             // 打开网络信息页面。
    }                                                        // 结束向下旋转处理。
  }                                                          // 结束旋转方向判断。

  if (controlWasShortPress() && !networkInfoVisible) {       // 设置列表短按进入当前项目编辑。
    editingSetting = true;                                   // 标记后续旋转将修改数值。
  }                                                          // 结束短按进入编辑处理。
  if (controlWasLongPress()) {                               // 设置页长按用于返回监测页。
    resetToMonitor();                                        // 统一恢复监测页状态。
  }                                                          // 结束长按返回处理。
}                                                            // 结束设置页输入处理函数。

}  // namespace  // 结束本源文件私有页面状态命名空间。

void begin() {        // 初始化显示硬件并呈现开机画面。
  oledInit();         // 初始化 OLED 所使用的 I2C 总线和 SSD1306 控制器。
  drawBootScreen();   // 在其他模块启动期间显示固定的启动信息。
}                     // 结束 OLED 控制模块初始化函数。

void update(Thresholds& thresholds, const SensorData& data,  // 接收当前设置和传感器数据。
            const EnvironmentStatus& status,                 // 接收主控制层计算后的环境状态。
            NetworkPortal& networkPortal) {                   // 接收网络页面及网络状态接口。
  if (networkPortal.takeConnectionSucceeded()) {             // 联网成功事件只消费一次。
    resetToMonitor();                                         // 联网完成后强制回到实时监测页。
  }                                                           // 结束联网成功事件处理。

  if (networkPortal.isShowingNetworkScreen()) {               // 配网或连接提示正在占用 OLED 时判断为真。
    return;                                                    // 不允许普通页面覆盖网络提示画面。
  }                                                            // 结束网络提示页面保护。

  const int rawDelta = controlReadEncoderDelta();             // 取走本轮旋钮累计增量。
  const int direction = rawDelta > 0 ? 1 : (rawDelta < 0 ? -1 : 0); // 把累计增量压缩为单步方向。

  if (screen == Screen::Monitor) {                            // 监测页只响应进入设置的长按事件。
    if (controlWasLongPress()) {                              // 长按旋钮表示打开设置页面。
      screen = Screen::Settings;                              // 把当前主页面切换为设置页。
      selectedSetting = kFirstSettingIndex;                   // 每次进入设置都从第一项开始。
      editingSetting = false;                                 // 确保刚进入时只移动光标而不编辑。
      networkInfoVisible = false;                             // 确保刚进入时显示设置列表。
    }                                                         // 结束监测页长按处理。
    drawMonitor(data, status);                                // 绘制两路光照、AQI、温湿度监测数据。
    return;                                                   // 监测页绘制完成后结束本轮显示更新。
  }                                                           // 结束监测页分支。

  updateSettings(thresholds, direction);                      // 在设置页处理旋钮导航和数值修改。
  const bool stationConnected = networkPortal.isStationConnected(); // 查询当前是否连接路由器。
  const String ipAddress = networkPortal.stationIpAddress();  // 获取设置页需要显示的设备 IP。
  const ReportUploadStatus reportStatus =                     // 获取数据及心跳上报状态快照。
      networkPortal.reportUploadStatus();                     // 完成本轮上报状态读取。
  drawSettings(thresholds, selectedSetting, editingSetting,       // 绘制不含实时数据的设置列表。
               ipAddress.c_str(), stationConnected, reportStatus, // 传入网络连接和上报状态。
               networkInfoVisible);                              // 传入当前是否显示网络信息页。
}                                                              // 结束 OLED 页面控制更新函数。

}  // namespace OledController  // 结束 OLED 页面控制实现命名空间。
