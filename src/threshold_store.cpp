/** @file threshold_store.cpp - 阈值的 NVS(掉电不丢的闪存) 持久化实现。 */
#include "threshold_store.h"   // 本模块对外声明的两个函数

#include <Preferences.h>       // ESP32 的 NVS 读写库
#include <math.h>              // 提供 isfinite()，判断浮点数是否为正常数值（非 NaN/无穷）

// 匿名命名空间：以下名字只在本文件内可见，等价于 C 语言的 static。
namespace {
constexpr char kPreferencesNamespace[] = "envboard";       // NVS 命名空间名字
constexpr char kThresholdsKey[] = "thresholds";            // 存阈值用的键名
// ENV7 增加独立的室外光照阈值，光敏2只负责判断是否允许自动照明。
constexpr uint32_t kThresholdsMagic = 0x454E5637;          // 当前版本魔数 "ENV7"，用来识别数据格式
constexpr uint32_t kLegacyThresholdsMagicV6 = 0x454E5636;  // 旧版本魔数 "ENV6"
constexpr uint32_t kLegacyThresholdsMagic = 0x454E5635;    // 更旧版本魔数 "ENV5"

// 旧版本 V5 的阈值字段（用于把老数据迁移到新格式）
struct LegacyThresholdsV5 {
  float lightHoldPercent;   // 自动照明保持的目标光照率
  float mq135Aqi;           // 空气质量阈值
  float temperatureC;       // 温度阈值
  float humidityPercent;    // 湿度阈值
};

// V5 在 NVS 里真正存储的结构：魔数 + 数据 + 校验和
struct StoredThresholdsV5 {
  uint32_t magic;               // 版本魔数
  LegacyThresholdsV5 values;    // 阈值数据
  uint32_t checksum;            // 校验和，用来验证数据没损坏
};

// 旧版本 V6 的阈值字段（比 V5 多了自动调光的起止时间）
struct LegacyThresholdsV6 {
  float lightHoldPercent;       // 自动照明目标光照率
  float mq135Aqi;               // 空气质量阈值
  float temperatureC;           // 温度阈值
  float humidityPercent;        // 湿度阈值
  uint16_t lightingStartMinute; // 自动调光开始时间（当天第几分钟）
  uint16_t lightingEndMinute;   // 自动调光结束时间
};

// V6 在 NVS 里真正存储的结构
struct StoredThresholdsV6 {
  uint32_t magic;               // 版本魔数
  LegacyThresholdsV6 values;    // 阈值数据
  uint32_t checksum;            // 校验和
};

// 当前版本在 NVS 里存储的结构（values 直接用共享的 Thresholds 结构体）
struct StoredThresholds {
  uint32_t magic;       // 版本魔数
  Thresholds values;    // 阈值数据
  uint32_t checksum;    // 校验和
};

Preferences preferences;    // 全局 NVS 操作对象
bool storeReady = false;    // 标记 NVS 是否已成功打开

// 计算一段内存的校验和（FNV-1a 哈希算法），用来检测数据是否被破坏
uint32_t checksumBytes(const void* value, size_t size) {
  // reinterpret_cast 是 C++ 的强制类型转换：把任意指针当成“逐字节”的指针来读
  const auto* bytes = reinterpret_cast<const uint8_t*>(value);
  uint32_t hash = 2166136261UL;      // FNV 初始值
  for (size_t i = 0; i < size; ++i) {// 逐字节遍历
    hash ^= bytes[i];                // 先异或当前字节
    hash *= 16777619UL;              // 再乘以 FNV 质数
  }
  return hash;                       // 返回最终哈希值
}

// 对一个 Thresholds 结构体求校验和
uint32_t checksumThresholds(const Thresholds& value) {
  return checksumBytes(&value, sizeof(value));   // 传入其地址和字节大小
}

// 检查阈值是否都在合理范围内（防止读到损坏或非法的数据）
bool thresholdsAreValid(const Thresholds& value) {
  return isfinite(value.outdoorLightThresholdPercent) && value.outdoorLightThresholdPercent >= 0.0f && value.outdoorLightThresholdPercent <= 100.0f &&
         isfinite(value.lightHoldPercent) && value.lightHoldPercent >= 0.0f && value.lightHoldPercent <= 100.0f &&
         isfinite(value.mq135Aqi) && value.mq135Aqi >= 0.0f && value.mq135Aqi <= 500.0f &&
         isfinite(value.temperatureC) && value.temperatureC >= 0.0f && value.temperatureC <= 60.0f &&
         isfinite(value.humidityPercent) && value.humidityPercent >= 0.0f && value.humidityPercent <= 100.0f &&
         value.lightingStartMinute < 24U * 60U && value.lightingEndMinute < 24U * 60U;  // 时间必须落在一天 1440 分钟内
}
}  // namespace   // 匿名命名空间结束

// 打开 NVS 并尝试读取已保存的阈值；成功读到有效数据返回 true
bool thresholdStoreBegin(Thresholds& thresholds) {
  storeReady = preferences.begin(kPreferencesNamespace, false);   // 以读写方式打开命名空间
  if (!storeReady) {                                              // 打开失败
    Serial.println("[NVS] ERROR: failed to open preferences namespace");
    return false;                                                 // 直接返回失败，保留默认阈值
  }

  const size_t storedSize = preferences.getBytesLength(kThresholdsKey);  // 先看已存数据有多大，用大小判断是哪个版本
  if (storedSize == sizeof(StoredThresholds)) {                          // 大小等于当前版本结构 → 按 ENV7 解析
    StoredThresholds stored{};                                          // {} 表示全部清零初始化
    const size_t bytesRead = preferences.getBytes(kThresholdsKey, &stored, sizeof(stored));  // 读出全部字节
    if (bytesRead == sizeof(stored) && stored.magic == kThresholdsMagic &&                   // 字节数对、魔数对
        stored.checksum == checksumThresholds(stored.values) &&                              // 校验和对
        thresholdsAreValid(stored.values)) {                                                 // 数据在合理范围
      thresholds = stored.values;                                                            // 全部条件满足才采用
      Serial.printf("[NVS] thresholds restored: outdoorLight=%.0f%% lightHold=%.0f%% schedule=%02u:%02u-%02u:%02u AQI=%.0f T=%.1fC H=%.0f%%\r\n",
                    thresholds.outdoorLightThresholdPercent,
                    thresholds.lightHoldPercent,
                    thresholds.lightingStartMinute / 60U,      // 分钟转小时
                    thresholds.lightingStartMinute % 60U,      // 分钟取余
                    thresholds.lightingEndMinute / 60U,
                    thresholds.lightingEndMinute % 60U,
                    thresholds.mq135Aqi,
                    thresholds.temperatureC, thresholds.humidityPercent);
      return true;                                                                           // 读取成功
    }
  } else if (storedSize == sizeof(StoredThresholdsV6)) {   // 大小等于 V6 → 迁移老数据
    StoredThresholdsV6 stored{};
    const size_t bytesRead = preferences.getBytes(kThresholdsKey, &stored, sizeof(stored));
    if (bytesRead == sizeof(stored) && stored.magic == kLegacyThresholdsMagicV6 &&           // 字节数、魔数对
        stored.checksum == checksumBytes(&stored.values, sizeof(stored.values))) {           // 校验和对
      thresholds.lightHoldPercent = stored.values.lightHoldPercent;      // 逐字段搬到新结构（缺的字段保留默认）
      thresholds.mq135Aqi = stored.values.mq135Aqi;
      thresholds.temperatureC = stored.values.temperatureC;
      thresholds.humidityPercent = stored.values.humidityPercent;
      thresholds.lightingStartMinute = stored.values.lightingStartMinute;
      thresholds.lightingEndMinute = stored.values.lightingEndMinute;
      if (thresholdsAreValid(thresholds)) {                              // 迁移后仍需校验范围
        Serial.println("[NVS] migrating ENV6 thresholds with default outdoor-light threshold");
        thresholdStoreSave(thresholds);                                  // 立刻用新格式重新保存一次
        return true;
      }
    }
  } else if (storedSize == sizeof(StoredThresholdsV5)) {   // 大小等于 V5 → 迁移更老的数据
    StoredThresholdsV5 stored{};
    const size_t bytesRead = preferences.getBytes(kThresholdsKey, &stored, sizeof(stored));
    if (bytesRead == sizeof(stored) && stored.magic == kLegacyThresholdsMagic &&             // 字节数、魔数对
        stored.checksum == checksumBytes(&stored.values, sizeof(stored.values))) {           // 校验和对
      thresholds.lightHoldPercent = stored.values.lightHoldPercent;      // 逐字段搬运，其余字段用默认
      thresholds.mq135Aqi = stored.values.mq135Aqi;
      thresholds.temperatureC = stored.values.temperatureC;
      thresholds.humidityPercent = stored.values.humidityPercent;
      if (thresholdsAreValid(thresholds)) {                              // 校验范围
        Serial.println("[NVS] migrating ENV5 thresholds with default lighting schedule and outdoor-light threshold");
        thresholdStoreSave(thresholds);                                  // 用新格式重新保存
        return true;
      }
    }
  }
  Serial.println("[NVS] no valid saved thresholds; using defaults");     // 没有任何有效数据
  return false;                                                          // 返回失败，使用默认阈值
}

// 把完整阈值写入 NVS，写入成功返回 true
bool thresholdStoreSave(const Thresholds& thresholds) {
  if (!storeReady) return false;    // NVS 没打开就直接失败
  // 组装成存储结构：魔数 + 数据 + 校验和（一行完成初始化）
  const StoredThresholds stored{kThresholdsMagic, thresholds, checksumThresholds(thresholds)};
  const size_t bytesWritten = preferences.putBytes(kThresholdsKey, &stored, sizeof(stored));  // 写入并返回写了多少字节
  if (bytesWritten != sizeof(stored)) {   // 写入的字节数不对 → 失败
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
  return true;                            // 写入成功
}
