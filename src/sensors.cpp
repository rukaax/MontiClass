/** @file sensors.cpp */
#include "sensors.h"

#include <math.h>

namespace {
// 多次读取取平均可抑制 ADC 和模块比较器附近的瞬时噪声。该函数没有
// delay，因此不会明显阻塞界面刷新。
constexpr uint8_t kAdcSamples = 8;
// 当前两只光敏模块按同一类型处理：AO 的 ADC 值在变暗时增大。若某一路
// 模块方向相反，只需修改对应常量。
constexpr bool kLight1DarkIsHigh = true;
constexpr bool kLight2DarkIsHigh = true;

struct AqiPoint {
  float rawRatio;
  float aqi;
};

// 相对空气指数曲线：本次开机基线对应 50，AO 相对基线升高时指数上升。
// 这是用于趋势和告警的 MQ135 估算值，不是由污染物浓度计算的标准 AQI。
constexpr AqiPoint kRelativeAqiCurve[] = {
    {0.0f, 0.0f}, {1.0f, 50.0f}, {1.25f, 100.0f}, {1.5f, 150.0f},
    {2.0f, 200.0f}, {3.0f, 300.0f}, {4.0f, 400.0f}, {5.0f, 500.0f},
};

float lightPercentFromRaw(uint16_t raw, bool darkIsHigh) {
  const float adcPercent = raw * 100.0f / 4095.0f;
  return darkIsHigh ? 100.0f - adcPercent : adcPercent;
}

float relativeAqiFromRaw(uint16_t raw, uint16_t baselineRaw) {
  if (baselineRaw == 0) return NAN;
  const float ratio = static_cast<float>(raw) / baselineRaw;
  for (size_t i = 1; i < sizeof(kRelativeAqiCurve) / sizeof(kRelativeAqiCurve[0]); ++i) {
    const AqiPoint& lower = kRelativeAqiCurve[i - 1];
    const AqiPoint& upper = kRelativeAqiCurve[i];
    if (ratio <= upper.rawRatio) {
      const float position = (ratio - lower.rawRatio) / (upper.rawRatio - lower.rawRatio);
      return constrain(lower.aqi + position * (upper.aqi - lower.aqi), 0.0f, 500.0f);
    }
  }
  return 500.0f;
}
}

void SensorManager::begin() {
  // ESP32-S3 ADC 设为 12 位，对应 0..4095。11 dB 衰减覆盖较宽
  // 的输入范围；无论如何，GPIO 上的电压都绝不能超过 3.3V。
  analogReadResolution(12);
  analogSetPinAttenuation(Pins::kLight1, ADC_11db);
  analogSetPinAttenuation(Pins::kLight2, ADC_11db);
  analogSetPinAttenuation(Pins::kMq135, ADC_11db);
  pinMode(Pins::kLight1, INPUT);
  pinMode(Pins::kLight2, INPUT);
  pinMode(Pins::kMq135, INPUT);
  pinMode(Pins::kLd2402, INPUT_PULLDOWN);
  // DHT 库自行完成 DATA 时序；这里只配置并记录 MQ135 预热起点。
  dht_.begin();
  mq135StartedMs_ = millis();
  mq135BaselineSum_ = 0;
  mq135BaselineSamples_ = 0;

}

bool SensorManager::update() {
  const uint32_t now = millis();
  // 人体存在信号每轮读取，不受两秒模拟量采样周期限制，保证 UI 响应及时。
  data_.presenceDetected = digitalRead(Pins::kLd2402) == HIGH;
  // DHT11 的推荐最短采样间隔约为 1 秒，这里使用 2 秒；sampledOnce_
  // 保证首次启动立即采样，而不是等待第一个完整周期。
  if (sampledOnce_ && now - lastSampleMs_ < kSampleIntervalMs) return false;
  sampledOnce_ = true;
  lastSampleMs_ = now;

  // 始终保留原始 ADC 值，现场可据此快速判断 AO 是否接错或悬空。
  data_.light1Raw = readAveragedAdc(Pins::kLight1);
  data_.light2Raw = readAveragedAdc(Pins::kLight2);
  data_.mq135Raw = readAveragedAdc(Pins::kMq135);
  data_.mq135MilliVolts = readAveragedMillivolts(Pins::kMq135);

  // 光敏百分比是相对亮度而非 lux。MQ135 在预热最后 60 秒采集本次
  // 开机基线，之后把 AO 相对基线的变化映射为 0..500 相对 AQI。
  data_.light1Percent = lightPercentFromRaw(data_.light1Raw, kLight1DarkIsHigh);
  data_.light2Percent = lightPercentFromRaw(data_.light2Raw, kLight2DarkIsHigh);
  const uint32_t mqElapsedMs = now - mq135StartedMs_;
  if (mqElapsedMs >= kMq135WarmupMs - kMq135BaselineWindowMs &&
      mqElapsedMs < kMq135WarmupMs) {
    mq135BaselineSum_ += data_.mq135Raw;
    ++mq135BaselineSamples_;
  }
  const bool wasWarmedUp = data_.mq135WarmedUp;
  data_.mq135WarmedUp = mqElapsedMs >= kMq135WarmupMs;
  if (data_.mq135WarmedUp && !wasWarmedUp) {
    data_.mq135BaselineRaw = mq135BaselineSamples_ == 0
                                 ? data_.mq135Raw
                                 : static_cast<uint16_t>(mq135BaselineSum_ / mq135BaselineSamples_);
  }
  if (data_.mq135WarmedUp) {
    data_.mq135AqiEstimate = relativeAqiFromRaw(data_.mq135Raw, data_.mq135BaselineRaw);
  }

  // DHT11 失败时不覆盖上一次有效温湿度，但 dhtValid 仍会变为 false，
  // 因而 OLED 显示 "--" 而不是把旧数据伪装成最新读数。
  const float temperature = dht_.readTemperature();
  const float humidity = dht_.readHumidity();
  data_.dhtValid = !isnan(temperature) && !isnan(humidity);
  if (data_.dhtValid) {
    data_.temperatureC = temperature;
    data_.humidityPercent = humidity;
  }

  return true;
}

const SensorData& SensorManager::data() const { return data_; }

uint16_t SensorManager::readAveragedAdc(uint8_t pin) {
  uint32_t sum = 0;
  // uint32_t 防止 8 个 12 位样本累加时发生 16 位溢出。
  for (uint8_t i = 0; i < kAdcSamples; ++i) sum += analogRead(pin);
  return static_cast<uint16_t>(sum / kAdcSamples);
}

uint16_t SensorManager::readAveragedMillivolts(uint8_t pin) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < kAdcSamples; ++i) sum += analogReadMilliVolts(pin);
  return static_cast<uint16_t>(sum / kAdcSamples);
}
