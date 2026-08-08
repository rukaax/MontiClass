/** @file buzzer.cpp */
#include "buzzer.h"          // 本模块自己的函数声明（头文件）

#include <Preferences.h>     // ESP32 的 NVS 存储库，用来把开关状态存到掉电不丢的闪存里

// C++ 里的“匿名命名空间”：把里面的东西限制在本文件内使用，
// 作用等于 C 语言里的 static，别的 .cpp 文件看不到这些名字。
namespace {
constexpr uint32_t kBuzzerOnMs = 50;              // 每次“滴”一声持续的毫秒数（constexpr = 编译期常量，相当于更安全的 #define）
constexpr uint16_t kBuzzerFrequencyHz = 2400;     // 蜂鸣器发声频率，2400Hz
constexpr char kPreferencesNamespace[] = "buzzer_cfg";  // NVS 里这块配置的命名空间名字
constexpr char kEnabledKey[] = "enabled";         // NVS 里“是否启用”这个键的名字

bool buzzerOn = false;            // 当前蜂鸣器是否正在响
bool toneStarted = false;         // 是否已经调用过 tone()（避免启动时误调 noTone 报错）
bool enabled = true;              // 报警功能总开关，默认开
uint8_t previousAlertMask = 0;    // 上一次的告警位（用来检测“新出现”的告警）
uint32_t beepStartedMs = 0;       // 本次鸣叫开始的时间戳（毫秒）

// 内部函数：打开或关闭蜂鸣器
void setBuzzer(bool on) {
  buzzerOn = on;                  // 记录当前状态
  if (on) {
    // 无源蜂鸣器需要方波；低电平触发只影响静音电平，方波相位不影响音调。
    tone(Pins::kBuzzer, kBuzzerFrequencyHz);   // 让引脚输出指定频率的方波，开始发声
    toneStarted = true;                        // 标记已经用过 tone()
  } else {
    // Arduino-ESP32 reports an LEDC error if noTone() is called before tone()
    // has allocated its channel during startup.
    if (toneStarted) {              // 只有真正响过，才需要调用 noTone 停止
      noTone(Pins::kBuzzer);        // 停止方波输出
      toneStarted = false;          // 复位标记
    }
    // 模块低电平触发，停止 PWM 后必须保持高电平，不能让输入悬空或拉低。
    digitalWrite(Pins::kBuzzer, HIGH);   // 拉高引脚，确保静音
  }
}
}  // namespace   // 匿名命名空间到此结束

// 初始化蜂鸣器：设置引脚、读取上次保存的开关状态
void buzzerInit() {
  // 先把输出锁存器置为高，再切为 OUTPUT，避免低电平触发模块在启动时误响。
  digitalWrite(Pins::kBuzzer, HIGH);   // 先写高电平
  pinMode(Pins::kBuzzer, OUTPUT);      // 再设为输出模式
  buzzerOn = false;                    // 初始状态：不响
  Preferences preferences;             // 创建一个 NVS 操作对象
  if (preferences.begin(kPreferencesNamespace, false)) {   // 以读写方式打开这块命名空间，成功返回 true
    enabled = preferences.getBool(kEnabledKey, true);      // 读取开关状态，读不到就用默认值 true
    preferences.end();                                     // 关闭 NVS，释放资源
  } else {
    enabled = true;                    // 打开失败就退回默认：开启
    Serial.println("[BUZZER] ERROR: failed to open NVS; using enabled default");  // 打印错误日志
  }
  // 打印一条初始化信息，%u 是无符号整数，%lu 是无符号长整数
  Serial.printf("[BUZZER] initialized gpio=%u type=passive trigger=active-low frequency=%uHz beep=%lums enabled=%u\r\n",
                Pins::kBuzzer, kBuzzerFrequencyHz,
                static_cast<unsigned long>(kBuzzerOnMs), enabled ? 1 : 0);  // static_cast 是 C++ 写法的强制类型转换
}

// 每轮主循环调用：根据告警位决定是否鸣叫；activeAlertMask 每个 bit 代表一类告警
void buzzerUpdate(uint8_t activeAlertMask) {
  const uint32_t now = millis();       // 取当前运行毫秒数
  // 找出“这次有、上次没有”的告警位：activeAlertMask 与 (上次取反) 做按位与
  const uint8_t newlyActiveAlerts =
      activeAlertMask & static_cast<uint8_t>(~previousAlertMask);
  previousAlertMask = activeAlertMask; // 更新“上一次”的记录

  if (!enabled) {                      // 如果总开关关闭
    if (buzzerOn) setBuzzer(false);    // 正在响就立刻停
    return;                            // 直接结束本次处理
  }

  if (newlyActiveAlerts != 0) {        // 有新出现的告警
    beepStartedMs = now;               // 记录鸣叫开始时间
    setBuzzer(true);                   // 开始响
    Serial.printf("[BUZZER] threshold crossed: new=0x%02X active=0x%02X\r\n",
                  newlyActiveAlerts, activeAlertMask);   // 打印哪些位触发了
  }

  if (buzzerOn && now - beepStartedMs >= kBuzzerOnMs) {  // 正在响且已经响够设定时长
    setBuzzer(false);                                    // 停止鸣叫
    Serial.println("[BUZZER] one-shot beep completed");  // 打印完成日志
  }
}

// 对外查询：报警功能当前是否启用
bool buzzerEnabled() { return enabled; }

// 对外设置：切换报警开关，只有成功写入 NVS 才真正生效
bool buzzerSetEnabled(bool nextEnabled) {
  if (nextEnabled == enabled) return true;   // 状态没变化，直接算成功

  Preferences preferences;                   // NVS 操作对象
  if (!preferences.begin(kPreferencesNamespace, false)) {   // 打开失败
    Serial.println("[BUZZER] ERROR: failed to open NVS while saving enabled state");
    return false;                            // 返回失败，不改状态
  }
  const bool saved = preferences.putBool(kEnabledKey, nextEnabled) == 1;  // 写入，返回写入的字节数，==1 表示成功
  preferences.end();                         // 关闭 NVS
  if (!saved) {                              // 写入失败
    Serial.println("[BUZZER] ERROR: failed to save enabled state");
    return false;                            // 返回失败，不改状态
  }

  enabled = nextEnabled;                     // 写入成功，更新内存里的状态
  if (!enabled) setBuzzer(false);            // 如果是关闭，立即停止当前鸣叫
  Serial.printf("[BUZZER] enabled=%u saved to NVS\r\n", enabled ? 1 : 0);  // 打印保存结果
  return true;                               // 返回成功
}
