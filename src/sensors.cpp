/** @file sensors.cpp */
#include "sensors.h"

#include <math.h>

namespace {
// 多次读取取平均可抑制 ADC 和模块比较器附近的瞬时噪声。该函数没有
// delay，因此不会明显阻塞界面刷新。
constexpr uint8_t kAdcSamples = 8;
// 原工程接法中，光敏 AO 的 ADC 值在变暗时增大。
constexpr bool kLightDarkIsHigh = true;
// 仅供 UI 显示和阈值比较的线性刻度。MQ135 的真实 ppm 与温湿度、
// 负载电阻和基线阻值有关，不能用这个常数替代正式标定。
constexpr float kMq135DisplayRangePpm = 2000.0f;
}

void SensorManager::begin() {
  // ESP32-S3 ADC 设为 12 位，对应 kAdcMax=4095。11 dB 衰减覆盖较宽
  // 的输入范围；无论如何，GPIO 上的电压都绝不能超过 3.3V。
  analogReadResolution(12);
  analogSetPinAttenuation(Pins::kLight, ADC_11db);
  analogSetPinAttenuation(Pins::kMq135, ADC_11db);
  pinMode(Pins::kLight, INPUT);
  pinMode(Pins::kMq135, INPUT);
  // DHT 库自行完成 DATA 时序；这里只配置并记录 MQ135 预热起点。
  dht_.begin();
  mq135StartedMs_ = millis();

  Serial.println("[sensor] initialized: light=GPIO7 MQ135=GPIO15 DHT11=GPIO16");
  Serial.println("[sensor] MQ135 is warming up; raw ADC is still reported for wiring checks.");
}

bool SensorManager::update() {
  const uint32_t now = millis();
  // DHT11 的推荐最短采样间隔约为 1 秒，这里使用 2 秒；sampledOnce_
  // 保证首次启动立即采样，而不是等待第一个完整周期。
  if (sampledOnce_ && now - lastSampleMs_ < kSampleIntervalMs) return false;
  sampledOnce_ = true;
  lastSampleMs_ = now;

  // 始终保留原始 ADC 值，现场可据此快速判断 AO 是否接错或悬空。
  data_.lightRaw = readAveragedAdc(Pins::kLight);
  data_.mq135Raw = readAveragedAdc(Pins::kMq135);

  // 光敏百分比是相对亮度而非 lux；若你的模块亮暗方向相反，修改
  // kLightDarkIsHigh 即可，不需要动 UI 和阈值代码。
  const float lightAdcPercent = data_.lightRaw * 100.0f / kAdcMax;
  data_.lightPercent = kLightDarkIsHigh ? 100.0f - lightAdcPercent : lightAdcPercent;
  data_.mq135PpmEstimate = data_.mq135Raw * kMq135DisplayRangePpm / kAdcMax;
  data_.mq135WarmedUp = now - mq135StartedMs_ >= kMq135WarmupMs;

  // DHT11 失败时不覆盖上一次有效温湿度，但 dhtValid 仍会变为 false，
  // 因而 OLED 显示 "--" 而不是把旧数据伪装成最新读数。
  const float temperature = dht_.readTemperature();
  const float humidity = dht_.readHumidity();
  data_.dhtValid = !isnan(temperature) && !isnan(humidity);
  if (data_.dhtValid) {
    data_.temperatureC = temperature;
    data_.humidityPercent = humidity;
  }

  printSample();
  return true;
}

const SensorData& SensorManager::data() const { return data_; }

uint16_t SensorManager::readAveragedAdc(uint8_t pin) {
  uint32_t sum = 0;
  // uint32_t 防止 8 个 12 位样本累加时发生 16 位溢出。
  for (uint8_t i = 0; i < kAdcSamples; ++i) sum += analogRead(pin);
  return static_cast<uint16_t>(sum / kAdcSamples);
}

void SensorManager::printSample() const {
  // 日志故意同时输出原始值和换算值：换算异常时可先判断是接线/ADC 问题
  // 还是上层的比例和阈值配置问题。
  // 每个分支只调用一次 printf 并包含换行，串口监视器将始终按完整行显示。
  // 使用栈上字符数组而非 Arduino String，避免长期输出日志产生堆碎片。
  char mqValue[12] = "warmup";
  if (data_.mq135WarmedUp) {
    snprintf(mqValue, sizeof(mqValue), "%.0fppm", data_.mq135PpmEstimate);
  }
  if (data_.dhtValid) {
    Serial.printf("[SENSOR] L=%u/%.0f%% MQ=%u/%s DHT=%.1fC/%.1f%%\n",
                  data_.lightRaw, data_.lightPercent, data_.mq135Raw,
                  mqValue,
                  data_.temperatureC, data_.humidityPercent);
  } else {
    Serial.printf("[SENSOR] L=%u/%.0f%% MQ=%u/%s DHT=ERROR\n",
                  data_.lightRaw, data_.lightPercent, data_.mq135Raw,
                  mqValue);
  }
}
