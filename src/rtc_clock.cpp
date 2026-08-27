#include "rtc_clock.h"          // 本模块的类声明和引脚定义

#include <Preferences.h>        // ESP32 的 NVS 存储库，用来记住 NTP 服务器地址
#include <Wire.h>               // Arduino I2C（两线）通信库
#include <esp_sntp.h>           // ESP-IDF 的 SNTP（网络时间）接口
#include <sys/time.h>           // settimeofday/gettimeofday 等系统时间函数

// 匿名命名空间：以下名字只在本文件内可见，等价于 C 里的 static
namespace {
constexpr char kPreferencesNamespace[] = "time_config";   // NVS 里这块配置的命名空间名
constexpr char kNtpServerKey[] = "ntp_server";            // NVS 里保存 NTP 服务器地址的键名
constexpr uint8_t kRtcAddress = 0x51;                     // PCF8563T 的 7 位 I2C 从机地址（8 位写地址为 0xA2）
constexpr uint8_t kRtcTimeRegister = 0x02;                // PCF8563T 时间寄存器从 VL_seconds 开始
constexpr time_t kMinimumValidEpoch = 1704067200;  // 2024-01-01 UTC   // 早于此时间戳视为无效时间
constexpr time_t kShanghaiOffsetSeconds = 8 * 3600;       // 东八区相对 UTC 的偏移（8 小时的秒数）
TwoWire rtcWire(1);                                       // 用 1 号 I2C 控制器单独给 RTC 用（和 OLED 分开）
volatile bool ntpSyncReceived = false;                    // volatile：会被中断/其他任务改动，禁止编译器优化缓存

// SNTP 收到时间同步时的回调函数
void onSntpSync(struct timeval*) {
  // The callback runs from the networking task; defer I2C access to loop().
  ntpSyncReceived = true;                                 // 只置个标志，真正的 I2C 操作留到 loop() 里做
}

// 探测 RTC 是否在总线上（发一个空传输看是否有应答）
bool rtcDevicePresent() {
  rtcWire.beginTransmission(kRtcAddress);                 // 开始向 RTC 地址发送
  return rtcWire.endTransmission() == 0;                  // 返回 0 表示对方应答成功，即设备在线
}

// 十进制转 BCD 码（PCF8563T 寄存器用 BCD 存储，比如 25 存成 0x25）
uint8_t toBcd(uint8_t value) { return static_cast<uint8_t>((value / 10U) * 16U + value % 10U); }
// BCD 码转回十进制
uint8_t fromBcd(uint8_t value) { return static_cast<uint8_t>((value >> 4U) * 10U + (value & 0x0fU)); }
// 判断是否闰年
bool isLeap(uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

// 把“本地时间(东八区)”结构体换算成 UTC 时间戳（自 1970 年起的秒数）
time_t makeLocalEpoch(const RtcDateTime& value) {
  int64_t days = 0;                                       // 累计从 1970 到目标年的总天数
  for (uint16_t year = 1970; year < value.year; ++year) days += isLeap(year) ? 366 : 365;  // 累加整年天数
  static const uint8_t monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};     // 各月天数（平年）
  for (uint8_t month = 1; month < value.month; ++month) {  // 累加当年已过完整月份的天数
    days += monthDays[month - 1];                          // 加上该月天数
    if (month == 2 && isLeap(value.year)) ++days;          // 闰年 2 月多补 1 天
  }
  days += value.day - 1;                                   // 加上当月已过的天数（当天不算满）
  const int64_t localSeconds = days * 86400LL + value.hour * 3600L + value.minute * 60L + value.second;  // 换算成本地总秒数
  return static_cast<time_t>(localSeconds - kShanghaiOffsetSeconds);   // 减去时区偏移，得到 UTC 时间戳
}

// 把 UTC 时间戳换算回“本地时间(东八区)”结构体
RtcDateTime fromLocal(time_t epoch) {
  struct tm tmValue{};                                     // 标准时间分解结构体，{} 表示全部清零
  const time_t shanghaiEpoch = epoch + kShanghaiOffsetSeconds;   // 先把 UTC 加上 8 小时变成本地时刻
  gmtime_r(&shanghaiEpoch, &tmValue);                      // 按 UTC 规则分解（因为已手动加过偏移），_r 版本线程安全
  RtcDateTime value;                                       // 目标结构体
  value.year = static_cast<uint16_t>(tmValue.tm_year + 1900);   // tm_year 是“距 1900 的年数”
  value.month = static_cast<uint8_t>(tmValue.tm_mon + 1);       // tm_mon 从 0 开始，要 +1
  value.day = static_cast<uint8_t>(tmValue.tm_mday);            // 日
  value.hour = static_cast<uint8_t>(tmValue.tm_hour);          // 时
  value.minute = static_cast<uint8_t>(tmValue.tm_min);         // 分
  value.second = static_cast<uint8_t>(tmValue.tm_sec);         // 秒
  return value;                                            // 返回换算结果
}
}  // namespace   // 匿名命名空间结束

RtcClock rtcClock;   // 全局唯一的时钟对象，供其他文件通过 extern 使用

String rtcI2cDevicesJson() {
  String result = "[";
  const uint16_t originalTimeout = rtcWire.getTimeOut();
  rtcWire.setTimeOut(20);
  bool first = true;
  for (uint8_t address = 0x08; address < 0x78; ++address) {
    rtcWire.beginTransmission(address);
    if (rtcWire.endTransmission() != 0) continue;
    if (!first) result += ',';
    char text[40];
    const char* module = address == kRtcAddress ? "PCF8563T RTC" : "未知模块";
    snprintf(text, sizeof(text), "\"0x%02X (%s)\"", address, module);
    result += text;
    first = false;
  }
  rtcWire.setTimeOut(originalTimeout);
  result += ']';
  return result;
}

// 类的初始化函数（RtcClock:: 表示这是 RtcClock 类的成员函数）
void RtcClock::begin() {
  rtcWire.begin(Pins::kRtcSda, Pins::kRtcScl, 100000);   // 启动 I2C：指定 SDA/SCL 引脚，速率 100kHz
  sntp_set_time_sync_notification_cb(onSntpSync);         // 注册 NTP 同步完成时的回调
  initialized_ = true;                                    // 标记已初始化（末尾下划线是私有成员命名习惯）
  loadNtpServer();                                        // 从 NVS 读回上次保存的 NTP 服务器地址
  RtcDateTime value;                                      // 临时存放读到的时间
  if (!rtcDevicePresent()) {                              // 如果总线上没检测到 RTC
    Serial.printf("[RTC] no I2C ACK at 0x%02X on SDA=%u SCL=%u; check VCC/GND/SDA/SCL and pull-ups\r\n",
                  kRtcAddress, Pins::kRtcSda, Pins::kRtcScl);   // 打印排查提示
  } else if (read(value)) {                               // 设备在线且成功读出时间
    setEpoch(makeLocalEpoch(value));                      // 用 RTC 的时间设置系统时钟
    Serial.printf("[RTC] PCF8563T I2C ready SDA=%u SCL=%u: %04u-%02u-%02u %02u:%02u:%02u\r\n",
                  Pins::kRtcSda, Pins::kRtcScl, value.year, value.month, value.day,
                  value.hour, value.minute, value.second);     // 打印当前时间
  } else {
    Serial.println("[RTC] PCF8563T not detected or date invalid; waiting for NTP/manual set");  // 读失败或时间非法
  }
}

// 校验一个日期时间是否合理（const 表示此函数不修改对象成员）
bool RtcClock::validDate(const RtcDateTime& value) const {
  if (value.year < 2024 || value.year > 2099 || value.month < 1 || value.month > 12 ||
      value.hour > 23 || value.minute > 59 || value.second > 59) return false;   // 任一字段越界即无效
  static const uint8_t days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};   // 各月最大天数（下标 1~12）
  const uint8_t maxDay = value.month == 2 && isLeap(value.year) ? 29 : days[value.month];   // 闰年 2 月按 29 天
  return value.day >= 1 && value.day <= maxDay;           // 日必须在合法范围内
}

// 从 PCF8563T 读取当前时间到 value
bool RtcClock::read(RtcDateTime& value) const {
  if (!initialized_ || !rtcDevicePresent()) return false;  // 未初始化或设备不在线，直接失败
  rtcWire.beginTransmission(kRtcAddress);                  // 开始通信
  rtcWire.write(kRtcTimeRegister);                         // 把寄存器指针指向 0x02（VL_seconds）
  const uint8_t registerResult = rtcWire.endTransmission(false);   // false=保持连接（重复起始），准备接着读
  if (registerResult != 0) return false;                   // 设置寄存器地址失败
  const uint8_t received = rtcWire.requestFrom(kRtcAddress, static_cast<uint8_t>(7));   // 请求读取 7 个字节（秒~年）
  uint8_t raw[7]{};                                        // 存放原始 BCD 数据，{} 清零
  for (uint8_t i = 0; i < received && i < sizeof(raw); ++i) raw[i] = rtcWire.read();    // 逐字节读入
  if (received != 7) return false;                         // 没读满 7 字节则失败
  if ((raw[0] & 0x80) != 0) return false;                  // VL=1 表示低电压后时钟数据不可信
  value.second = fromBcd(raw[0] & 0x7f);                   // 秒：低 7 位有效
  value.minute = fromBcd(raw[1] & 0x7f);                   // 分：低 7 位有效
  value.hour = fromBcd(raw[2] & 0x3f);                     // PCF8563T 只使用 24 小时制
  value.day = fromBcd(raw[3] & 0x3f);                      // 日（raw[4] 是星期，跳过不用）
  value.month = fromBcd(raw[5] & 0x1f);                    // 月：低 5 位（bit7 是世纪位，忽略）
  value.year = static_cast<uint16_t>(2000 + fromBcd(raw[6]));   // 年：寄存器存的是 00~99，加上 2000
  return validDate(value);                                 // 最后校验读出的日期是否合理
}

// 把 value 写入 PCF8563T（设置时间）
bool RtcClock::set(const RtcDateTime& value) {
  if (!initialized_ || !validDate(value) || !rtcDevicePresent()) return false;   // 未初始化/日期非法/设备离线则失败
  const uint8_t raw[] = {toBcd(value.second), toBcd(value.minute), toBcd(value.hour),
                         toBcd(value.day), 1, toBcd(value.month),
                         toBcd(static_cast<uint8_t>(value.year - 2000))};   // 转成寄存器顺序的 BCD 数组（星期固定填1）
  rtcWire.beginTransmission(kRtcAddress);                  // 开始通信
  rtcWire.write(kRtcTimeRegister);                         // 指向 VL_seconds 寄存器
  for (uint8_t item : raw) rtcWire.write(item);            // 范围 for：依次写入全部 7 个字节
  const uint8_t writeResult = rtcWire.endTransmission();   // 结束并发送
  if (writeResult != 0) return false;                      // 写入失败
  setEpoch(makeLocalEpoch(value));                         // 同步更新系统时钟
  return true;                                             // 成功
}

// 用 UTC 时间戳设置系统时钟
bool RtcClock::setEpoch(time_t epoch) {
  if (epoch < kMinimumValidEpoch) return false;            // 早于 2024 视为无效，拒绝
  struct timeval tv{epoch, 0};                             // timeval{秒, 微秒}
  return settimeofday(&tv, nullptr) == 0;                  // 设置系统时间，返回 0 表示成功
}

// 读取当前时间戳到 epoch
bool RtcClock::nowEpoch(time_t& epoch) const {
  RtcDateTime value;                                       // 临时时间
  if (!read(value)) return false;                          // 读 RTC 失败
  epoch = makeLocalEpoch(value);                           // 换算成时间戳
  return epoch >= kMinimumValidEpoch;                      // 必须不早于最小有效时间
}

// 判断当前时间是否有效
bool RtcClock::isValid() const {
  time_t epoch = 0;                                        // 临时变量
  return nowEpoch(epoch);                                  // 能成功取到有效时间戳即为有效
}

// 返回 ISO 8601 格式的时间字符串（带 +08:00 时区）
String RtcClock::isoString() const {
  RtcDateTime value;                                       // 临时时间
  if (!read(value)) return String();                       // 读失败返回空字符串
  char text[24];                                           // 存放格式化结果的缓冲区
  snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u+08:00", value.year, value.month,
           value.day, value.hour, value.minute, value.second);   // 格式化拼字符串
  return String(text);                                     // 转成 Arduino 的 String 返回
}

// 从 NVS 载入上次保存的 NTP 服务器地址
void RtcClock::loadNtpServer() {
  Preferences preferences;                                 // NVS 操作对象
  if (!preferences.begin(kPreferencesNamespace, true)) return;   // 以只读方式打开，失败就直接返回（用默认值）
  const String configured = preferences.getString(kNtpServerKey, ntpServer_);   // 读取，读不到就用当前默认值
  preferences.end();                                       // 关闭 NVS
  if (!configured.isEmpty() && configured.length() <= 128) ntpServer_ = configured;   // 合法才采用
}

// 设置 NTP 服务器地址，persist=是否写入 NVS 永久保存
bool RtcClock::setNtpServer(const String& server, bool persist) {
  String value = server;                                   // 拷贝一份用于处理
  value.trim();                                            // 去掉首尾空白
  if (value.isEmpty() || value.length() > 128) return false;   // 空或过长则拒绝
  for (size_t i = 0; i < value.length(); ++i) {            // 逐字符校验合法性
    const char c = value[i];                               // 当前字符
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':';   // 只允许字母数字和 . - :
    if (!allowed) return false;                            // 出现非法字符即拒绝
  }
  if (persist) {                                           // 需要持久化
    Preferences preferences;                               // NVS 对象
    if (!preferences.begin(kPreferencesNamespace, false)) return false;   // 以读写方式打开，失败返回
    const bool saved = preferences.putString(kNtpServerKey, value) == value.length();   // 写入，返回写入长度，相等才算成功
    preferences.end();                                     // 关闭 NVS
    if (!saved) return false;                              // 写入失败
  }
  ntpServer_ = value;                                      // 更新内存中的地址
  ntpStarted_ = false;                                     // 重置状态，促使下次重新发起同步
  ntpPending_ = false;                                     // 清除“等待中”标志
  ntpRetryAtMs_ = 0;                                       // 清除重试时间
  ntpSyncReceived = false;                                 // 清除同步收到标志
  return true;                                             // 成功
}

// 定期推进 NTP 同步流程，networkConnected=当前是否联网
void RtcClock::updateNtp(bool networkConnected) {
  if (!initialized_ || !networkConnected) return;          // 未初始化或没网就不处理
  if (!ntpStarted_ && ntpRetryAtMs_ != 0 && static_cast<int32_t>(millis() - ntpRetryAtMs_) < 0) return;   // 还没到重试时间就等待（用有符号差值防溢出）
  if (!ntpStarted_) {                                      // 还没开始同步
    ntpSyncReceived = false;                               // 清标志
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);          // 复位 SNTP 同步状态
    configTime(0, 0, ntpServer_.c_str());                 // 启动 NTP：时区偏移都填 0（我们自己处理时区）
    ntpStarted_ = true;                                    // 标记已启动
    ntpPending_ = true;                                    // 标记等待结果中
    ntpStartedMs_ = millis();                              // 记录启动时刻，用于超时判断
    Serial.printf("[RTC] NTP sync started: %s\r\n", ntpServer_.c_str());   // 打印日志
  }
  const time_t epoch = time(nullptr);                      // 取当前系统时间戳
  if (ntpSyncReceived && epoch >= kMinimumValidEpoch) {    // 收到同步且时间有效
    ntpSyncReceived = false;                               // 清标志
    if (set(fromLocal(epoch))) {                           // 把网络时间写进 PCF8563T
      Serial.printf("[RTC] NTP synchronized: %s\r\n", isoString().c_str());   // 成功
    } else {
      Serial.println("[RTC] NTP response received, but PCF8563T write failed");  // 写 RTC 失败
    }
    ntpPending_ = false;                                   // 结束等待
  } else if (millis() - ntpStartedMs_ > 30000) {           // 超过 30 秒还没同步成功
    if (ntpPending_) Serial.printf("[RTC] NTP sync timeout: %s\r\n", ntpServer_.c_str());   // 打印超时
    ntpPending_ = false;                                   // 清等待标志
    ntpStarted_ = false;                                   // 允许重新发起
    ntpRetryAtMs_ = millis() + 10000;                      // 10 秒后再重试
  }
}
