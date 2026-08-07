/**
 * @file sensors.h
 * @brief 两路光敏、MQ135、DHT11 与 LD2402 的数据结构和采集接口声明。
 *
 * GPIO 初始化和实际读取实现集中在 src/main.cpp，便于初学者顺序阅读。
 */
#pragma once

#include <Arduino.h>
#include <DHT.h>

namespace Pins {
// AO 必须接在可用 ADC 的 GPIO 上，且输入电压不得超过 3.3V。
constexpr uint8_t kLight1 = 7;      // 第一只光敏模块 AO
constexpr uint8_t kLight2 = 6;      // 第二只光敏模块 AO（ADC1_CH5）
constexpr uint8_t kMq135 = 15;      // MQ135 模块 AO
constexpr uint8_t kDht11 = 16;      // DHT11 DATA
constexpr uint8_t kLd2402 = 10;     // LD2402 人体存在数字输出：高=有人，低=无人
}  // namespace Pins

struct SensorData {
  uint16_t light1Raw = 0;           // 0..4095，方便验线与校准
  uint16_t light2Raw = 0;           // 0..4095，方便验线与校准
  uint16_t mq135Raw = 0;            // 0..4095，方便验线与校准
  uint16_t mq135MilliVolts = 0;     // 经 ESP32 ADC 校准后的 AO 近似电压
  uint16_t mq135BaselineRaw = 0;    // 预热末段建立的本次开机空气基线
  float light1Percent = 0.0f;       // 0..100，数值越大越亮
  float light2Percent = 0.0f;       // 0..100，数值越大越亮
  float mq135AqiEstimate = NAN;     // 0..500 相对 AQI，不等同于国家标准 AQI
  // NAN 表示从未读到有效 DHT 数据；使用 dhtValid 判断“本轮是否成功”。
  float temperatureC = NAN;
  float humidityPercent = NAN;
  bool dhtValid = false;
  bool mq135WarmedUp = false;
  bool presenceDetected = false;
};

class SensorManager {
 public:
  // 配置 ADC、启动 DHT 库，并开始 MQ135 的预热计时。
  void begin();
  // 每 2 秒采样一次；本轮发生采样时返回 true。
  bool update();
  // 返回只读引用，避免在每帧 OLED 刷新时复制整个数据结构。
  const SensorData& data() const;

 private:
  // 采样周期与 DHT11 的限制兼容，也使串口日志便于阅读。
  static constexpr uint32_t kSampleIntervalMs = 2000;
  // 每次冷启动至少等待 3 分钟再显示响应强度。新传感器首次使用仍应按
  // 所购模块资料进行更长时间老化；真实 ppm 还必须单独标定 R0 和气体曲线。
  static constexpr uint32_t kMq135WarmupMs = 180000;
  static constexpr uint32_t kMq135BaselineWindowMs = 60000;

  DHT dht_{Pins::kDht11, DHT11};
  SensorData data_{};
  uint32_t lastSampleMs_ = 0;
  uint32_t mq135StartedMs_ = 0;
  uint32_t mq135BaselineSum_ = 0;
  uint16_t mq135BaselineSamples_ = 0;
  bool sampledOnce_ = false;

  static uint16_t readAveragedAdc(uint8_t pin);
  static uint16_t readAveragedMillivolts(uint8_t pin);
};
