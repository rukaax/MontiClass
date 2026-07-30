/**
 * 教室环境监测器。
 * 接线：OLED SDA=17/SCL=18；EC11 CLK=4/DT=5/SW=8；
 *       光敏 AO=7；MQ135 AO=15；DHT11 DATA=16。
 */
#include <Arduino.h>
#include <Preferences.h>

#include <math.h>

#include "SSD1306.h"
#include "control.h"
#include "sensors.h"

namespace {
// 两个页面共用同一个主循环。不要在旋钮/传感器函数里直接切页面，
// 否则按键事件会被消费两次，状态会难以追踪。
enum class Screen : uint8_t { Monitor, Settings };

// 静态对象在 setup() 前构造；真正的硬件初始化均在 setup() 中完成。
SensorManager sensors;
// 阈值在启动时从 NVS 恢复；若 NVS 没有有效记录才使用这些默认值。
Thresholds thresholds;
// Preferences 是 ESP32 Arduino 自带的 NVS（非易失性存储）接口。
// 该命名空间独立于 Wi-Fi 等系统配置，不会和其它数据冲突。
Preferences preferences;
constexpr char kPreferencesNamespace[] = "envboard";
constexpr char kThresholdsKey[] = "thresholds";
constexpr uint32_t kThresholdsMagic = 0x454E5631;  // "ENV1"

// 单个 blob 由 NVS 原子写入；magic + 校验和可过滤尺寸不匹配、旧版本或
// 意外损坏的数据，避免用异常阈值驱动界面。
struct StoredThresholds {
  uint32_t magic;
  Thresholds values;
  uint32_t checksum;
};

bool thresholdsDirty = false;
Screen screen = Screen::Monitor;
// 0..3 与 OLED 设置页的 光照/空气/温度/湿度 四行一一对应。
uint8_t selectedThreshold = 0;
// false 表示“移动光标”，true 表示“旋转会修改当前数值”。
bool editingThreshold = false;

uint32_t checksumThresholds(const Thresholds& value) {
  // FNV-1a：这里不是安全校验，只用于发现闪存数据损坏或结构版本不匹配。
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < sizeof(value); ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

bool thresholdsAreValid(const Thresholds& value) {
  // 不只校验 checksum：旧固件或异常写入也不能把不合理数据恢复到 UI。
  return isfinite(value.lightPercent) && value.lightPercent >= 0.0f && value.lightPercent <= 100.0f &&
         isfinite(value.mq135Ppm) && value.mq135Ppm >= 0.0f && value.mq135Ppm <= 2000.0f &&
         isfinite(value.temperatureC) && value.temperatureC >= 0.0f && value.temperatureC <= 60.0f &&
         isfinite(value.humidityPercent) && value.humidityPercent >= 0.0f && value.humidityPercent <= 100.0f;
}

void loadThresholds() {
  StoredThresholds stored{};
  const size_t bytesRead = preferences.getBytes(kThresholdsKey, &stored, sizeof(stored));
  if (bytesRead == sizeof(stored) && stored.magic == kThresholdsMagic &&
      stored.checksum == checksumThresholds(stored.values) && thresholdsAreValid(stored.values)) {
    thresholds = stored.values;
    Serial.printf("[NVS] thresholds restored: L=%.0f%% MQ=%.0fppm T=%.1fC H=%.0f%%\n",
                  thresholds.lightPercent, thresholds.mq135Ppm,
                  thresholds.temperatureC, thresholds.humidityPercent);
  } else {
    // 没有第一次启动的数据、或数据不完整时继续使用 Thresholds 的默认值。
    Serial.println("[NVS] no valid saved thresholds; using defaults");
  }
}

void saveThresholds() {
  if (!thresholdsDirty) return;
  const StoredThresholds stored{kThresholdsMagic, thresholds, checksumThresholds(thresholds)};
  const size_t bytesWritten = preferences.putBytes(kThresholdsKey, &stored, sizeof(stored));
  if (bytesWritten == sizeof(stored)) {
    thresholdsDirty = false;
    Serial.printf("[NVS] thresholds saved: L=%.0f%% MQ=%.0fppm T=%.1fC H=%.0f%%\n",
                  thresholds.lightPercent, thresholds.mq135Ppm,
                  thresholds.temperatureC, thresholds.humidityPercent);
  } else {
    Serial.println("[NVS] ERROR: threshold save failed");
  }
}

// direction 只会是 -1、0、1。即使用户快速旋转很多格，主循环也
// 有意每轮只改一次，避免阈值突然跳变得不可控。
void adjustSelectedThreshold(int direction) {
  switch (selectedThreshold) {
    case 0:
      // 光照、湿度均为百分比，限制在显示和物理意义一致的 0..100。
      thresholds.lightPercent = constrain(thresholds.lightPercent + direction, 0.0f, 100.0f);
      break;
    case 1:
      // MQ135 的映射范围是 0..2000 ppm，因此以 10 ppm 为调节粒度。
      thresholds.mq135Ppm = constrain(thresholds.mq135Ppm + direction * 10.0f, 0.0f, 2000.0f);
      break;
    case 2:
      // 温度保留一位小数，半度步进能兼顾精度和旋钮操作速度。
      thresholds.temperatureC = constrain(thresholds.temperatureC + direction * 0.5f, 0.0f, 60.0f);
      break;
    case 3:
      thresholds.humidityPercent = constrain(thresholds.humidityPercent + direction, 0.0f, 100.0f);
      break;
  }
  // 不在这里保存。旋转中的每一格只改 RAM，短按确认时才写一次闪存。
  thresholdsDirty = true;
}

// 设置页分成“选择”和“编辑”两态，避免旋转时误改阈值。
void updateSettings(int direction) {
  if (editingThreshold) {
    if (direction != 0) adjustSelectedThreshold(direction);
    // 短按既是“确认”，也是唯一的闪存提交点。
    if (controlWasShortPress()) {
      saveThresholds();
      editingThreshold = false;
    }
    return;
  }

  // 正负方向的页面移动与实际旋钮手感保持一致；如安装方向相反，
  // 应先在 control.cpp 的中断中处理，而不是在这里反转业务语义。
  if (direction > 0 && selectedThreshold > 0) --selectedThreshold;
  if (direction < 0 && selectedThreshold < 3) ++selectedThreshold;
  if (controlWasShortPress()) editingThreshold = true;
  if (controlWasLongPress()) screen = Screen::Monitor;
}
}  // namespace

void setup() {
  // 串口用于现场验线：采样值及读失败原因都会每 2 秒打印一次。
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[boot] Classroom ESP32 monitor");

  if (!preferences.begin(kPreferencesNamespace, false)) {
    Serial.println("[NVS] ERROR: failed to open preferences namespace");
  } else {
    loadThresholds();
  }

  // OLED 先启动，方便即便某个传感器未接好时也能显示启动状态。
  oledInit();
  drawBootScreen();
  // 输入必须在传感器前初始化：用户可在 MQ135 预热期间直接操作界面。
  controlInit();
  sensors.begin();
  delay(800);
}

void loop() {
  // update() 内部限频至 2 秒；此处每轮调用不会造成 DHT11 过度读取。
  sensors.update();
  // 按键消抖必须高频执行，不能与 2 秒采样绑定。
  controlUpdate();

  // 中断可能累计多个编码器边沿。页面逻辑只关心本轮的方向，
  // 将其压缩为 -1/0/1 可以避免机械抖动导致一次跨越多项。
  const int rawDelta = controlReadEncoderDelta();
  const int direction = rawDelta > 0 ? 1 : (rawDelta < 0 ? -1 : 0);

  if (screen == Screen::Monitor) {
    // 主页面仅允许长按进入设置，短按不会干扰实时监测。
    if (controlWasLongPress()) {
      screen = Screen::Settings;
      selectedThreshold = 0;
      editingThreshold = false;
    }
    drawMonitor(sensors.data(), thresholds);
  } else {
    updateSettings(direction);
    drawSettings(thresholds, selectedThreshold, editingThreshold);
  }
  // 调试输出放在主循环末尾：可安全打印 EC11 状态，且不干扰 ISR 解码。
  controlDebugPrint();
  // 约 66 Hz 刷新输入与画面；无阻塞式传感器等待。
  delay(15);
}
