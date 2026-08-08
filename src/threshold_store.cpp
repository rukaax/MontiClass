/** @file threshold_store.cpp */
#include "threshold_store.h"

#include <Preferences.h>
#include <math.h>

namespace {
constexpr char kPreferencesNamespace[] = "envboard";
constexpr char kThresholdsKey[] = "thresholds";
// ENV7 增加独立的室外光照阈值，光敏2只负责判断是否允许自动照明。
constexpr uint32_t kThresholdsMagic = 0x454E5637;  // "ENV7"
constexpr uint32_t kLegacyThresholdsMagicV6 = 0x454E5636;  // "ENV6"
constexpr uint32_t kLegacyThresholdsMagic = 0x454E5635;  // "ENV5"

struct LegacyThresholdsV5 {
  float lightHoldPercent;
  float mq135Aqi;
  float temperatureC;
  float humidityPercent;
};

struct StoredThresholdsV5 {
  uint32_t magic;
  LegacyThresholdsV5 values;
  uint32_t checksum;
};

struct LegacyThresholdsV6 {
  float lightHoldPercent;
  float mq135Aqi;
  float temperatureC;
  float humidityPercent;
  uint16_t lightingStartMinute;
  uint16_t lightingEndMinute;
};

struct StoredThresholdsV6 {
  uint32_t magic;
  LegacyThresholdsV6 values;
  uint32_t checksum;
};

struct StoredThresholds {
  uint32_t magic;
  Thresholds values;
  uint32_t checksum;
};

Preferences preferences;
bool storeReady = false;

uint32_t checksumBytes(const void* value, size_t size) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(value);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

uint32_t checksumThresholds(const Thresholds& value) {
  return checksumBytes(&value, sizeof(value));
}

bool thresholdsAreValid(const Thresholds& value) {
  return isfinite(value.outdoorLightThresholdPercent) && value.outdoorLightThresholdPercent >= 0.0f && value.outdoorLightThresholdPercent <= 100.0f &&
         isfinite(value.lightHoldPercent) && value.lightHoldPercent >= 0.0f && value.lightHoldPercent <= 100.0f &&
         isfinite(value.mq135Aqi) && value.mq135Aqi >= 0.0f && value.mq135Aqi <= 500.0f &&
         isfinite(value.temperatureC) && value.temperatureC >= 0.0f && value.temperatureC <= 60.0f &&
         isfinite(value.humidityPercent) && value.humidityPercent >= 0.0f && value.humidityPercent <= 100.0f &&
         value.lightingStartMinute < 24U * 60U && value.lightingEndMinute < 24U * 60U;
}
}  // namespace

bool thresholdStoreBegin(Thresholds& thresholds) {
  storeReady = preferences.begin(kPreferencesNamespace, false);
  if (!storeReady) {
    Serial.println("[NVS] ERROR: failed to open preferences namespace");
    return false;
  }

  const size_t storedSize = preferences.getBytesLength(kThresholdsKey);
  if (storedSize == sizeof(StoredThresholds)) {
    StoredThresholds stored{};
    const size_t bytesRead = preferences.getBytes(kThresholdsKey, &stored, sizeof(stored));
    if (bytesRead == sizeof(stored) && stored.magic == kThresholdsMagic &&
        stored.checksum == checksumThresholds(stored.values) &&
        thresholdsAreValid(stored.values)) {
      thresholds = stored.values;
      Serial.printf("[NVS] thresholds restored: outdoorLight=%.0f%% lightHold=%.0f%% schedule=%02u:%02u-%02u:%02u AQI=%.0f T=%.1fC H=%.0f%%\r\n",
                    thresholds.outdoorLightThresholdPercent,
                    thresholds.lightHoldPercent,
                    thresholds.lightingStartMinute / 60U,
                    thresholds.lightingStartMinute % 60U,
                    thresholds.lightingEndMinute / 60U,
                    thresholds.lightingEndMinute % 60U,
                    thresholds.mq135Aqi,
                    thresholds.temperatureC, thresholds.humidityPercent);
      return true;
    }
  } else if (storedSize == sizeof(StoredThresholdsV6)) {
    StoredThresholdsV6 stored{};
    const size_t bytesRead = preferences.getBytes(kThresholdsKey, &stored, sizeof(stored));
    if (bytesRead == sizeof(stored) && stored.magic == kLegacyThresholdsMagicV6 &&
        stored.checksum == checksumBytes(&stored.values, sizeof(stored.values))) {
      thresholds.lightHoldPercent = stored.values.lightHoldPercent;
      thresholds.mq135Aqi = stored.values.mq135Aqi;
      thresholds.temperatureC = stored.values.temperatureC;
      thresholds.humidityPercent = stored.values.humidityPercent;
      thresholds.lightingStartMinute = stored.values.lightingStartMinute;
      thresholds.lightingEndMinute = stored.values.lightingEndMinute;
      if (thresholdsAreValid(thresholds)) {
        Serial.println("[NVS] migrating ENV6 thresholds with default outdoor-light threshold");
        thresholdStoreSave(thresholds);
        return true;
      }
    }
  } else if (storedSize == sizeof(StoredThresholdsV5)) {
    StoredThresholdsV5 stored{};
    const size_t bytesRead = preferences.getBytes(kThresholdsKey, &stored, sizeof(stored));
    if (bytesRead == sizeof(stored) && stored.magic == kLegacyThresholdsMagic &&
        stored.checksum == checksumBytes(&stored.values, sizeof(stored.values))) {
      thresholds.lightHoldPercent = stored.values.lightHoldPercent;
      thresholds.mq135Aqi = stored.values.mq135Aqi;
      thresholds.temperatureC = stored.values.temperatureC;
      thresholds.humidityPercent = stored.values.humidityPercent;
      if (thresholdsAreValid(thresholds)) {
        Serial.println("[NVS] migrating ENV5 thresholds with default lighting schedule and outdoor-light threshold");
        thresholdStoreSave(thresholds);
        return true;
      }
    }
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
  Serial.printf("[NVS] thresholds saved: outdoorLight=%.0f%% lightHold=%.0f%% schedule=%02u:%02u-%02u:%02u AQI=%.0f T=%.1fC H=%.0f%%\r\n",
                thresholds.outdoorLightThresholdPercent,
                thresholds.lightHoldPercent,
                thresholds.lightingStartMinute / 60U,
                thresholds.lightingStartMinute % 60U,
                thresholds.lightingEndMinute / 60U,
                thresholds.lightingEndMinute % 60U,
                thresholds.mq135Aqi,
                thresholds.temperatureC, thresholds.humidityPercent);
  return true;
}
