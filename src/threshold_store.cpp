/** @file threshold_store.cpp */
#include "threshold_store.h"

#include <Preferences.h>
#include <math.h>

namespace {
constexpr char kPreferencesNamespace[] = "envboard";
constexpr char kThresholdsKey[] = "thresholds";
// ENV3 将空气阈值改为 0..500 相对 AQI，旧百分比阈值不可直接复用。
constexpr uint32_t kThresholdsMagic = 0x454E5633;  // "ENV3"

struct StoredThresholds {
  uint32_t magic;
  Thresholds values;
  uint32_t checksum;
};

Preferences preferences;
bool storeReady = false;

uint32_t checksumThresholds(const Thresholds& value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < sizeof(value); ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

bool thresholdsAreValid(const Thresholds& value) {
  return isfinite(value.lightPercent) && value.lightPercent >= 0.0f && value.lightPercent <= 100.0f &&
         isfinite(value.mq135Aqi) && value.mq135Aqi >= 0.0f && value.mq135Aqi <= 500.0f &&
         isfinite(value.temperatureC) && value.temperatureC >= 0.0f && value.temperatureC <= 60.0f &&
         isfinite(value.humidityPercent) && value.humidityPercent >= 0.0f && value.humidityPercent <= 100.0f;
}
}  // namespace

bool thresholdStoreBegin(Thresholds& thresholds) {
  storeReady = preferences.begin(kPreferencesNamespace, false);
  if (!storeReady) {
    Serial.println("[NVS] ERROR: failed to open preferences namespace");
    return false;
  }

  StoredThresholds stored{};
  const size_t bytesRead = preferences.getBytes(kThresholdsKey, &stored, sizeof(stored));
  if (bytesRead == sizeof(stored) && stored.magic == kThresholdsMagic &&
      stored.checksum == checksumThresholds(stored.values) && thresholdsAreValid(stored.values)) {
    thresholds = stored.values;
    Serial.printf("[NVS] thresholds restored: L=%.0f%% AQI=%.0f T=%.1fC H=%.0f%%\r\n",
                  thresholds.lightPercent, thresholds.mq135Aqi,
                  thresholds.temperatureC, thresholds.humidityPercent);
    return true;
  }
  Serial.println("[NVS] no valid saved thresholds; using defaults");
  return false;
}

bool thresholdStoreSave(const Thresholds& thresholds) {
  if (!storeReady) return false;
  const StoredThresholds stored{kThresholdsMagic, thresholds, checksumThresholds(thresholds)};
  const size_t bytesWritten = preferences.putBytes(kThresholdsKey, &stored, sizeof(stored));
  if (bytesWritten != sizeof(stored)) {
    Serial.println("[NVS] ERROR: threshold save failed");
    return false;
  }
  Serial.printf("[NVS] thresholds saved: L=%.0f%% AQI=%.0f T=%.1fC H=%.0f%%\r\n",
                thresholds.lightPercent, thresholds.mq135Aqi,
                thresholds.temperatureC, thresholds.humidityPercent);
  return true;
}
