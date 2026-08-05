#include "rtc_clock.h"

#include <Preferences.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <sys/time.h>

namespace {
constexpr char kPreferencesNamespace[] = "time_config";
constexpr char kNtpServerKey[] = "ntp_server";
constexpr uint8_t kRtcAddress = 0x68;
constexpr time_t kMinimumValidEpoch = 1704067200;  // 2024-01-01 UTC
constexpr time_t kShanghaiOffsetSeconds = 8 * 3600;
TwoWire rtcWire(1);
volatile bool ntpSyncReceived = false;

void onSntpSync(struct timeval*) {
  // The callback runs from the networking task; defer I2C access to loop().
  ntpSyncReceived = true;
}

bool rtcDevicePresent() {
  rtcWire.beginTransmission(kRtcAddress);
  return rtcWire.endTransmission() == 0;
}

uint8_t toBcd(uint8_t value) { return static_cast<uint8_t>((value / 10U) * 16U + value % 10U); }
uint8_t fromBcd(uint8_t value) { return static_cast<uint8_t>((value >> 4U) * 10U + (value & 0x0fU)); }
bool isLeap(uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

time_t makeLocalEpoch(const RtcDateTime& value) {
  int64_t days = 0;
  for (uint16_t year = 1970; year < value.year; ++year) days += isLeap(year) ? 366 : 365;
  static const uint8_t monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  for (uint8_t month = 1; month < value.month; ++month) {
    days += monthDays[month - 1];
    if (month == 2 && isLeap(value.year)) ++days;
  }
  days += value.day - 1;
  const int64_t localSeconds = days * 86400LL + value.hour * 3600L + value.minute * 60L + value.second;
  return static_cast<time_t>(localSeconds - kShanghaiOffsetSeconds);
}

RtcDateTime fromLocal(time_t epoch) {
  struct tm tmValue{};
  const time_t shanghaiEpoch = epoch + kShanghaiOffsetSeconds;
  gmtime_r(&shanghaiEpoch, &tmValue);
  RtcDateTime value;
  value.year = static_cast<uint16_t>(tmValue.tm_year + 1900);
  value.month = static_cast<uint8_t>(tmValue.tm_mon + 1);
  value.day = static_cast<uint8_t>(tmValue.tm_mday);
  value.hour = static_cast<uint8_t>(tmValue.tm_hour);
  value.minute = static_cast<uint8_t>(tmValue.tm_min);
  value.second = static_cast<uint8_t>(tmValue.tm_sec);
  return value;
}
}  // namespace

RtcClock rtcClock;

void RtcClock::begin() {
  rtcWire.begin(Pins::kRtcSda, Pins::kRtcScl, 100000);
  sntp_set_time_sync_notification_cb(onSntpSync);
  initialized_ = true;
  loadNtpServer();
  RtcDateTime value;
  if (!rtcDevicePresent()) {
    Serial.printf("[RTC] no I2C ACK at 0x%02X on SDA=%u SCL=%u; check VCC/GND/SDA/SCL and pull-ups\r\n",
                  kRtcAddress, Pins::kRtcSda, Pins::kRtcScl);
  } else if (read(value)) {
    setEpoch(makeLocalEpoch(value));
    Serial.printf("[RTC] DS3231 I2C ready SDA=%u SCL=%u: %04u-%02u-%02u %02u:%02u:%02u\r\n",
                  Pins::kRtcSda, Pins::kRtcScl, value.year, value.month, value.day,
                  value.hour, value.minute, value.second);
  } else {
    Serial.println("[RTC] DS3231 not detected or date invalid; waiting for NTP/manual set");
  }
}

bool RtcClock::validDate(const RtcDateTime& value) const {
  if (value.year < 2024 || value.year > 2099 || value.month < 1 || value.month > 12 ||
      value.hour > 23 || value.minute > 59 || value.second > 59) return false;
  static const uint8_t days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const uint8_t maxDay = value.month == 2 && isLeap(value.year) ? 29 : days[value.month];
  return value.day >= 1 && value.day <= maxDay;
}

bool RtcClock::read(RtcDateTime& value) const {
  if (!initialized_ || !rtcDevicePresent()) return false;
  rtcWire.beginTransmission(kRtcAddress);
  rtcWire.write(0x00);
  const uint8_t registerResult = rtcWire.endTransmission(false);
  if (registerResult != 0) return false;
  const uint8_t received = rtcWire.requestFrom(kRtcAddress, static_cast<uint8_t>(7));
  uint8_t raw[7]{};
  for (uint8_t i = 0; i < received && i < sizeof(raw); ++i) raw[i] = rtcWire.read();
  if (received != 7) return false;
  if (!rtcDevicePresent()) return false;
  rtcWire.beginTransmission(kRtcAddress);
  rtcWire.write(0x0f);
  const uint8_t statusRegisterResult = rtcWire.endTransmission(false);
  if (statusRegisterResult != 0) return false;
  const uint8_t statusReceived = rtcWire.requestFrom(kRtcAddress, static_cast<uint8_t>(1));
  uint8_t statusRaw = statusReceived == 1 ? rtcWire.read() : 0;
  if (statusReceived != 1 || (statusRaw & 0x80) != 0) return false;
  value.second = fromBcd(raw[0] & 0x7f);
  value.minute = fromBcd(raw[1] & 0x7f);
  if ((raw[2] & 0x40) != 0) {
    value.hour = fromBcd(raw[2] & 0x1f);
    if ((raw[2] & 0x20) != 0 && value.hour < 12) value.hour += 12;
    else if ((raw[2] & 0x20) == 0 && value.hour == 12) value.hour = 0;
  } else {
    value.hour = fromBcd(raw[2] & 0x3f);
  }
  value.day = fromBcd(raw[4] & 0x3f);
  value.month = fromBcd(raw[5] & 0x1f);
  value.year = static_cast<uint16_t>(2000 + fromBcd(raw[6]));
  return validDate(value);
}

bool RtcClock::set(const RtcDateTime& value) {
  if (!initialized_ || !validDate(value) || !rtcDevicePresent()) return false;
  const uint8_t raw[] = {toBcd(value.second), toBcd(value.minute), toBcd(value.hour), 1,
                         toBcd(value.day), toBcd(value.month), toBcd(static_cast<uint8_t>(value.year - 2000))};
  rtcWire.beginTransmission(kRtcAddress);
  rtcWire.write(0x00);
  for (uint8_t item : raw) rtcWire.write(item);
  const uint8_t writeResult = rtcWire.endTransmission();
  if (writeResult != 0) return false;
  if (!rtcDevicePresent()) return false;
  rtcWire.beginTransmission(kRtcAddress);
  rtcWire.write(0x0f);
  const uint8_t statusRegisterResult = rtcWire.endTransmission(false);
  if (statusRegisterResult == 0) {
    const uint8_t statusReceived = rtcWire.requestFrom(kRtcAddress, static_cast<uint8_t>(1));
    const uint8_t status = statusReceived == 1 ? rtcWire.read() : 0;
    if (statusReceived == 1) {
      rtcWire.beginTransmission(kRtcAddress);
      rtcWire.write(0x0f);
      rtcWire.write(status & ~0x80);
      rtcWire.endTransmission();
    }
  }
  setEpoch(makeLocalEpoch(value));
  return true;
}

bool RtcClock::setEpoch(time_t epoch) {
  if (epoch < kMinimumValidEpoch) return false;
  struct timeval tv{epoch, 0};
  return settimeofday(&tv, nullptr) == 0;
}

bool RtcClock::nowEpoch(time_t& epoch) const {
  RtcDateTime value;
  if (!read(value)) return false;
  epoch = makeLocalEpoch(value);
  return epoch >= kMinimumValidEpoch;
}

bool RtcClock::isValid() const {
  time_t epoch = 0;
  return nowEpoch(epoch);
}

String RtcClock::isoString() const {
  RtcDateTime value;
  if (!read(value)) return String();
  char text[24];
  snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u+08:00", value.year, value.month,
           value.day, value.hour, value.minute, value.second);
  return String(text);
}

void RtcClock::loadNtpServer() {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, true)) return;
  const String configured = preferences.getString(kNtpServerKey, ntpServer_);
  preferences.end();
  if (!configured.isEmpty() && configured.length() <= 128) ntpServer_ = configured;
}

bool RtcClock::setNtpServer(const String& server, bool persist) {
  String value = server;
  value.trim();
  if (value.isEmpty() || value.length() > 128) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':';
    if (!allowed) return false;
  }
  if (persist) {
    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, false)) return false;
    const bool saved = preferences.putString(kNtpServerKey, value) == value.length();
    preferences.end();
    if (!saved) return false;
  }
  ntpServer_ = value;
  ntpStarted_ = false;
  ntpPending_ = false;
  ntpRetryAtMs_ = 0;
  ntpSyncReceived = false;
  return true;
}

void RtcClock::updateNtp(bool networkConnected) {
  if (!initialized_ || !networkConnected) return;
  if (!ntpStarted_ && ntpRetryAtMs_ != 0 && static_cast<int32_t>(millis() - ntpRetryAtMs_) < 0) return;
  if (!ntpStarted_) {
    ntpSyncReceived = false;
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    configTime(0, 0, ntpServer_.c_str());
    ntpStarted_ = true;
    ntpPending_ = true;
    ntpStartedMs_ = millis();
    Serial.printf("[RTC] NTP sync started: %s\r\n", ntpServer_.c_str());
  }
  const time_t epoch = time(nullptr);
  if (ntpSyncReceived && epoch >= kMinimumValidEpoch) {
    ntpSyncReceived = false;
    if (set(fromLocal(epoch))) {
      Serial.printf("[RTC] NTP synchronized: %s\r\n", isoString().c_str());
    } else {
      Serial.println("[RTC] NTP response received, but DS3231 write failed");
    }
    ntpPending_ = false;
  } else if (millis() - ntpStartedMs_ > 30000) {
    if (ntpPending_) Serial.printf("[RTC] NTP sync timeout: %s\r\n", ntpServer_.c_str());
    ntpPending_ = false;
    ntpStarted_ = false;
    ntpRetryAtMs_ = millis() + 10000;
  }
}
