/** @file report_uploader.h
 * @brief 传感器数据的 HTTP 定时上报与 WebUI 持久化配置。
 */
#pragma once

#include <Arduino.h>
#include <HTTPClient.h>

#include "sensors.h"

class WebServer;

class ReportUploader {
 public:
  void begin();
  void configureRoutes(WebServer& server);
  void update(const SensorData& data);

 private:
  static constexpr uint32_t kDefaultReportIntervalMs = 10000;
  static constexpr uint32_t kDefaultHeartbeatIntervalMs = 60000;
  static constexpr uint8_t kLogCapacity = 12;

  struct LogEntry {
    uint32_t timeMs = 0;
    int code = 0;
    String kind;
    String response;
    size_t requestBytes = 0;
  };

  void sendConfiguration();
  void receiveConfiguration();
  void receiveManualReport();
  void sendLogs();
  void clearLogs();
  bool loadConfiguration();
  bool saveConfiguration(const String& baseUrl, const String& token,
                         uint32_t reportIntervalMs, uint32_t heartbeatIntervalMs,
                         const String& customHeaders);
  bool validateCustomHeaders(const String& headers) const;
  void applyCustomHeaders(HTTPClient& http) const;
  void appendLog(const char* kind, int code, const String& response, size_t requestBytes);
  int upload(const SensorData& data, const char* kind, bool includeSensorData);
  String reportsUrl() const;

  WebServer* server_ = nullptr;
  String baseUrl_;
  String token_;
  String deviceId_;
  String customHeaders_;
  uint32_t reportIntervalMs_ = kDefaultReportIntervalMs;
  uint32_t heartbeatIntervalMs_ = kDefaultHeartbeatIntervalMs;
  uint32_t lastReportMs_ = 0;
  uint32_t lastHeartbeatMs_ = 0;
  int lastHttpCode_ = 0;
  bool reportAttempted_ = false;
  bool heartbeatAttempted_ = false;
  bool manualRequested_ = false;
  LogEntry logs_[kLogCapacity];
  uint8_t logCount_ = 0;
  uint8_t logNext_ = 0;
};
