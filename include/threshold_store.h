/** @file threshold_store.h
 * @brief 阈值的 NVS 持久化接口。
 */
#pragma once

#include "thresholds.h"

// 打开 NVS 并读取有效的已保存阈值；失败或没有记录时保留默认值。
bool thresholdStoreBegin(Thresholds& thresholds);
// 将完整阈值集合写入 NVS。返回值表示本次写入是否成功。
bool thresholdStoreSave(const Thresholds& thresholds);
