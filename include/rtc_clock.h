/** @file rtc_clock.h
 * @brief DS3231SN two-wire I2C RTC and NTP synchronization.
 */
#pragma once

#include <Arduino.h>
#include <time.h>

namespace Pins {
// Dedicated RTC I2C bus. GPIO35-37 are reserved by OPI PSRAM on N8R8 boards.
// OLED remains on the independent GPIO17/18 bus.
constexpr uint8_t kRtcSda = 47;
constexpr uint8_t kRtcScl = 48;
}  // namespace Pins

struct RtcDateTime {
  uint16_t year = 2000;
  uint8_t month = 1;
  uint8_t day = 1;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
};

class RtcClock {
 public:
  void begin();
  bool read(RtcDateTime& value) const;
  bool set(const RtcDateTime& value);
  bool setEpoch(time_t epoch);
  bool nowEpoch(time_t& epoch) const;
  bool isValid() const;
  String isoString() const;
  bool setNtpServer(const String& server, bool persist = true);
  const String& ntpServer() const { return ntpServer_; }
  void updateNtp(bool networkConnected);
  bool ntpSyncPending() const { return ntpPending_; }

 private:
  bool validDate(const RtcDateTime& value) const;
  void loadNtpServer();
  String ntpServer_ = "pool.ntp.org";
  bool initialized_ = false;
  bool ntpPending_ = false;
  bool ntpStarted_ = false;
  uint32_t ntpStartedMs_ = 0;
  uint32_t ntpRetryAtMs_ = 0;
};

extern RtcClock rtcClock;
