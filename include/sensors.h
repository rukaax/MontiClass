/**
 * @file sensors.h
 * @brief 光敏、MQ135 与 DHT11 的统一采集接口。
 */
#pragma once

#include <Arduino.h>
#include <DHT.h>

namespace Pins {
// AO 必须接在可用 ADC 的 GPIO 上，且输入电压不得超过 3.3V。
constexpr uint8_t kLight = 7;       // 光敏模块 AO
constexpr uint8_t kMq135 = 15;      // MQ135 模块 AO
constexpr uint8_t kDht11 = 16;      // DHT11 DATA
}  // namespace Pins

struct SensorData {
  uint16_t lightRaw = 0;            // 0..4095，方便验线与校准
  uint16_t mq135Raw = 0;            // 0..4095，方便验线与校准
  float lightPercent = 0.0f;        // 0..100，数值越大越亮
  float mq135PpmEstimate = 0.0f;    // 未校准的线性估算值，不是实验室浓度
  // NAN 表示从未读到有效 DHT 数据；使用 dhtValid 判断“本轮是否成功”。
  float temperatureC = NAN;
  float humidityPercent = NAN;
  bool dhtValid = false;
  bool mq135WarmedUp = false;
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
  // 该时间仅用于 UI 提示和基础稳定；正式气体测量仍需按模块资料校准。
  static constexpr uint32_t kMq135WarmupMs = 30000;
  static constexpr uint16_t kAdcMax = 4095;

  DHT dht_{Pins::kDht11, DHT11};
  SensorData data_{};
  uint32_t lastSampleMs_ = 0;
  uint32_t mq135StartedMs_ = 0;
  bool sampledOnce_ = false;

  static uint16_t readAveragedAdc(uint8_t pin);
  void printSample() const;
};
