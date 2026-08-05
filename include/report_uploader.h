/** @file report_uploader.h
 * @brief 传感器数据的 HTTP 定时上报与 WebUI 持久化配置。
 */
#pragma once

#include <Arduino.h>

#include "sensors.h"

class WebServer;

enum class ApiConnectivity : uint8_t {
  NotConfigured,
  Pending,
  Reachable,
  Unreachable,
};

struct ReportUploadStatus {
  bool hasDataUpload = false;
  uint32_t lastDataUploadMs = 0;
  bool hasHeartbeatUpload = false;
  uint32_t lastHeartbeatUploadMs = 0;
  ApiConnectivity apiConnectivity = ApiConnectivity::NotConfigured;
};

class ReportUploader {
 public:
  void begin();
  void configureRoutes(WebServer& server);
  void update(const SensorData& data);
  ReportUploadStatus status() const;

 private:
  static constexpr uint32_t kDefaultReportIntervalMs = 10000;
  static constexpr uint32_t kDefaultHeartbeatIntervalMs = 60000;
  static constexpr uint8_t kLogCapacity = 12;
  static constexpr uint8_t kSuccessLogCapacity = 5;

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
                         uint32_t reportIntervalMs, uint32_t heartbeatIntervalMs);
  String buildRequest(const SensorData& data, const String& host, const String& path) const;
  bool ensureTlsTime();
  void appendLog(const char* kind, int code, const String& response, size_t requestBytes);
  int upload(const SensorData& data, const char* kind);

  WebServer* server_ = nullptr;
  String baseUrl_;
  String token_;
  String deviceId_;
  uint32_t reportIntervalMs_ = kDefaultReportIntervalMs;
  uint32_t heartbeatIntervalMs_ = kDefaultHeartbeatIntervalMs;
  uint32_t lastReportMs_ = 0;
  uint32_t lastHeartbeatMs_ = 0;
  int lastHttpCode_ = 0;
  bool reportAttempted_ = false;
  bool heartbeatAttempted_ = false;
  bool manualRequested_ = false;
  bool hasDataUpload_ = false;
  bool hasHeartbeatUpload_ = false;
  uint32_t lastDataUploadMs_ = 0;
  uint32_t lastHeartbeatUploadMs_ = 0;
  bool timeSyncStarted_ = false;
  bool timeSyncFailureLogged_ = false;
  bool timeSyncCompleteLogged_ = false;
  uint32_t timeSyncStartedMs_ = 0;
  uint32_t uploadNotBeforeMs_ = 0;
  LogEntry logs_[kLogCapacity];
  uint8_t logCount_ = 0;
};
