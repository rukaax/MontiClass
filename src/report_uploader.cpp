/** @file report_uploader.cpp */
#include "report_uploader.h"
#include "rtc_clock.h"

#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <math.h>
#include <time.h>

extern const uint8_t rootca_crt_bundle_start[]
    asm("_binary_data_cert_x509_crt_bundle_bin_start");

namespace {
constexpr char kPreferencesNamespace[] = "report_cfg";
constexpr char kBaseUrlKey[] = "base_url";
constexpr char kTokenKey[] = "token";
constexpr char kReportIntervalKey[] = "report_ms";
constexpr char kHeartbeatIntervalKey[] = "heartbeat_ms";
constexpr char kObsoleteHeadersKey[] = "headers";
constexpr char kObsoleteRequestTemplateKey[] = "request_tpl";
constexpr size_t kMaxBaseUrlLength = 240;
constexpr size_t kMaxTokenLength = 512;
constexpr size_t kMaxLogResponseLength = 256;
constexpr size_t kMaxSuccessLogResponseLength = 64;
constexpr uint32_t kMinReportIntervalMs = 1000;
constexpr uint32_t kMaxIntervalMs = 86400000UL;
constexpr uint32_t kConnectTimeoutMs = 1500;
constexpr uint32_t kIoTimeoutMs = 2000;
constexpr uint32_t kTimeSyncWarningMs = 20000;
constexpr time_t kMinimumValidEpoch = 1704067200;  // 2024-01-01 UTC
constexpr int kTransportConnectionFailed = -1;
constexpr int kTransportWriteFailed = -3;
constexpr int kTransportInvalidResponse = -9;
constexpr int kTransportReadTimeout = -11;
constexpr int kTransportTimeUnavailable = -13;

String escapeJson(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 4);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '"' || c == '\\') escaped += '\\';
    if (c == '\n') escaped += "\\n";
    else if (c == '\r') escaped += "\\r";
    else if (static_cast<uint8_t>(c) >= 0x20) escaped += c;
  }
  return escaped;
}

bool parseHttpUrl(const String& value, String& host, uint16_t& port,
                  String& hostHeader, String& path, bool& secure) {
  size_t schemeLength = 0;
  if (value.startsWith("https://")) {
    secure = true;
    schemeLength = 8;
  } else if (value.startsWith("http://")) {
    secure = false;
    schemeLength = 7;
  } else {
    return false;
  }
  if (value.length() > kMaxBaseUrlLength || value.length() <= schemeLength ||
      value.indexOf('"') >= 0 ||
      value.indexOf('\\') >= 0 || value.indexOf('?') >= 0 ||
      value.indexOf('#') >= 0) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (static_cast<uint8_t>(value[i]) < 0x20 || value[i] == ' ') return false;
  }

  const int pathStart = value.indexOf('/', schemeLength);
  const String authority = pathStart < 0 ? value.substring(schemeLength)
                                         : value.substring(schemeLength, pathStart);
  if (authority.isEmpty() || authority.indexOf('@') >= 0) return false;
  hostHeader = authority;
  port = secure ? 443 : 80;

  if (authority.startsWith("[")) {
    const int closingBracket = authority.indexOf(']');
    if (closingBracket <= 1) return false;
    host = authority.substring(1, closingBracket);
    if (closingBracket + 1 < authority.length()) {
      if (authority[closingBracket + 1] != ':') return false;
      const String portText = authority.substring(closingBracket + 2);
      if (portText.isEmpty()) return false;
      char* end = nullptr;
      const unsigned long parsedPort = strtoul(portText.c_str(), &end, 10);
      if (*end != '\0' || parsedPort == 0 || parsedPort > 65535) return false;
      port = static_cast<uint16_t>(parsedPort);
    }
  } else {
    const int colon = authority.lastIndexOf(':');
    if (colon >= 0) {
      if (authority.indexOf(':') != colon) return false;
      host = authority.substring(0, colon);
      const String portText = authority.substring(colon + 1);
      if (host.isEmpty() || portText.isEmpty()) return false;
      char* end = nullptr;
      const unsigned long parsedPort = strtoul(portText.c_str(), &end, 10);
      if (*end != '\0' || parsedPort == 0 || parsedPort > 65535) return false;
      port = static_cast<uint16_t>(parsedPort);
    } else {
      host = authority;
    }
  }
  if (host.isEmpty()) return false;

  path = pathStart < 0 ? "" : value.substring(pathStart);
  if (path.isEmpty()) path = "/reports";
  else if (!path.endsWith("/reports")) path += "/reports";
  return true;
}

bool validBaseUrl(const String& value) {
  String host;
  String hostHeader;
  String path;
  uint16_t port = 0;
  bool secure = false;
  return parseHttpUrl(value, host, port, hostHeader, path, secure);
}

bool validToken(const String& value) {
  if (value.length() > kMaxTokenLength) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (static_cast<uint8_t>(value[i]) < 0x20 || value[i] == 0x7f) return false;
  }
  return true;
}

bool systemTimeValid() { return rtcClock.isValid(); }

bool successfulHttpCode(int code) {
  return code >= 200 && code < 300;
}

bool parseIntervalSeconds(const String& text, uint32_t minimumSeconds,
                          uint32_t& milliseconds) {
  if (text.isEmpty()) return false;
  char* end = nullptr;
  const unsigned long seconds = strtoul(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || seconds < minimumSeconds ||
      seconds > (kMaxIntervalMs / 1000UL)) return false;
  milliseconds = static_cast<uint32_t>(seconds * 1000UL);
  return true;
}

bool waitForResponseData(WiFiClient& client, uint32_t timeoutMs) {
  const uint32_t started = millis();
  while (client.available() == 0) {
    if (!client.connected() || millis() - started >= timeoutMs) return false;
    delay(1);
  }
  return true;
}

bool writeEntireRequest(WiFiClient& client, const String& request) {
  size_t written = 0;
  uint32_t lastProgress = millis();
  while (written < request.length()) {
    const size_t count = client.write(
        reinterpret_cast<const uint8_t*>(request.c_str()) + written,
        request.length() - written);
    if (count > 0) {
      written += count;
      lastProgress = millis();
    } else if (!client.connected() || millis() - lastProgress >= kIoTimeoutMs) {
      return false;
    } else {
      delay(1);
    }
  }
  return true;
}

int readHttpResponse(WiFiClient& client, String& response) {
  if (!waitForResponseData(client, kIoTimeoutMs)) {
    response = "response timeout";
    return kTransportReadTimeout;
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  const int firstSpace = statusLine.indexOf(' ');
  if (!statusLine.startsWith("HTTP/") || firstSpace < 0 || statusLine.length() < firstSpace + 4) {
    response = "invalid HTTP response";
    return kTransportInvalidResponse;
  }
  const int statusCode = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();
  if (statusCode < 100 || statusCode > 999) {
    response = "invalid HTTP status";
    return kTransportInvalidResponse;
  }

  int contentLength = -1;
  while (true) {
    if (!waitForResponseData(client, kIoTimeoutMs)) {
      response = "response header timeout";
      return statusCode;
    }
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) break;
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      const String lengthText = line.substring(line.indexOf(':') + 1);
      contentLength = lengthText.toInt();
      if (contentLength < 0) contentLength = -1;
    }
  }

  response = "";
  size_t received = 0;
  uint32_t lastData = millis();
  while (contentLength < 0 || received < static_cast<size_t>(contentLength)) {
    bool readAny = false;
    while (client.available() > 0) {
      const int value = client.read();
      if (value < 0) break;
      if (response.length() < kMaxLogResponseLength) response += static_cast<char>(value);
      ++received;
      readAny = true;
    }
    if (contentLength >= 0 && received >= static_cast<size_t>(contentLength)) break;
    if (readAny) lastData = millis();
    else if (!client.connected() || millis() - lastData >= kIoTimeoutMs) break;
    else delay(1);
  }
  return statusCode;
}
}  // namespace

void ReportUploader::begin() {
  char deviceId[32];
  const uint64_t chipId = ESP.getEfuseMac() & 0x0000FFFFFFFFFFFFULL;
  snprintf(deviceId, sizeof(deviceId), "esp32s3-%012llX",
           static_cast<unsigned long long>(chipId));
  deviceId_ = deviceId;
  loadConfiguration();
  lastHeartbeatMs_ = millis();
  heartbeatAttempted_ = heartbeatIntervalMs_ > 0;
  Serial.printf("[REPORT] device_id=%s configured=%s report=%lus heartbeat=%lus\r\n",
                deviceId_.c_str(), !baseUrl_.isEmpty() ? "yes" : "no",
                static_cast<unsigned long>(reportIntervalMs_ / 1000),
                static_cast<unsigned long>(heartbeatIntervalMs_ / 1000));
}

void ReportUploader::configureRoutes(WebServer& server) {
  server_ = &server;
  server.on("/api/reporting", HTTP_GET, [this]() { sendConfiguration(); });
  server.on("/api/reporting", HTTP_POST, [this]() { receiveConfiguration(); });
  server.on("/api/reporting/send", HTTP_POST, [this]() { receiveManualReport(); });
  server.on("/api/reporting/logs", HTTP_GET, [this]() { sendLogs(); });
  server.on("/api/reporting/logs/clear", HTTP_POST, [this]() { clearLogs(); });
}

bool ReportUploader::loadConfiguration() {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;
  baseUrl_ = preferences.getString(kBaseUrlKey, "");
  token_ = preferences.getString(kTokenKey, "");
  reportIntervalMs_ = preferences.getULong(kReportIntervalKey, kDefaultReportIntervalMs);
  heartbeatIntervalMs_ = preferences.getULong(kHeartbeatIntervalKey, kDefaultHeartbeatIntervalMs);
  if (preferences.isKey(kObsoleteHeadersKey)) preferences.remove(kObsoleteHeadersKey);
  if (preferences.isKey(kObsoleteRequestTemplateKey)) {
    preferences.remove(kObsoleteRequestTemplateKey);
  }
  preferences.end();
  if (reportIntervalMs_ < kMinReportIntervalMs || reportIntervalMs_ > kMaxIntervalMs) {
    reportIntervalMs_ = kDefaultReportIntervalMs;
  }
  if (heartbeatIntervalMs_ > kMaxIntervalMs) heartbeatIntervalMs_ = kDefaultHeartbeatIntervalMs;
  return !baseUrl_.isEmpty();
}

bool ReportUploader::saveConfiguration(const String& baseUrl, const String& token,
                                       uint32_t reportIntervalMs, uint32_t heartbeatIntervalMs) {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;
  const bool baseSaved = preferences.putString(kBaseUrlKey, baseUrl) == baseUrl.length();
  bool tokenSaved = true;
  if (!token.isEmpty()) tokenSaved = preferences.putString(kTokenKey, token) == token.length();
  const bool reportSaved = preferences.putULong(kReportIntervalKey, reportIntervalMs) != 0;
  const bool heartbeatSaved = preferences.putULong(kHeartbeatIntervalKey, heartbeatIntervalMs) != 0;
  preferences.end();
  if (!baseSaved || !tokenSaved || !reportSaved || !heartbeatSaved) return false;
  baseUrl_ = baseUrl;
  if (!token.isEmpty()) token_ = token;
  reportIntervalMs_ = reportIntervalMs;
  heartbeatIntervalMs_ = heartbeatIntervalMs;
  return true;
}

void ReportUploader::sendConfiguration() {
  if (server_ == nullptr) return;
  const bool configured = !baseUrl_.isEmpty();
  const ReportUploadStatus uploadStatus = status();
  const char* apiConnectivity = "pending";
  if (uploadStatus.apiConnectivity == ApiConnectivity::NotConfigured) {
    apiConnectivity = "not_configured";
  } else if (uploadStatus.apiConnectivity == ApiConnectivity::Reachable) {
    apiConnectivity = "reachable";
  } else if (uploadStatus.apiConnectivity == ApiConnectivity::Unreachable) {
    apiConnectivity = "unreachable";
  }
  String response = "{\"deviceId\":\"" + escapeJson(deviceId_) +
                    "\",\"baseUrl\":\"" + escapeJson(baseUrl_) +
                    "\",\"tokenConfigured\":" + (token_.isEmpty() ? "false" : "true") +
                    ",\"configured\":" + (configured ? "true" : "false") +
                    ",\"reportIntervalSec\":" + String(reportIntervalMs_ / 1000) +
                    ",\"heartbeatIntervalSec\":" + String(heartbeatIntervalMs_ / 1000) +
                    ",\"secure\":" + (baseUrl_.startsWith("https://") ? "true" : "false") +
                    ",\"tlsTimeReady\":" + (systemTimeValid() ? "true" : "false") +
                    ",\"wifiConnected\":" +
                    (WiFi.status() == WL_CONNECTED ? "true" : "false") +
                    ",\"uptimeMs\":" + String(millis()) +
                    ",\"hasDataUpload\":" +
                    (uploadStatus.hasDataUpload ? "true" : "false") +
                    ",\"lastDataUploadMs\":" + String(uploadStatus.lastDataUploadMs) +
                    ",\"hasHeartbeatUpload\":" +
                    (uploadStatus.hasHeartbeatUpload ? "true" : "false") +
                    ",\"lastHeartbeatUploadMs\":" +
                    String(uploadStatus.lastHeartbeatUploadMs) +
                    ",\"apiConnectivity\":\"" + apiConnectivity + "\"" +
                    ",\"lastHttpCode\":" + String(lastHttpCode_) + "}";
  server_->sendHeader("Cache-Control", "no-store");
  server_->send(200, "application/json", response);
}

void ReportUploader::receiveConfiguration() {
  if (server_ == nullptr) return;
  String baseUrl = server_->arg("base_url");
  const String token = server_->arg("token");
  String reportText = server_->arg("report_interval");
  String heartbeatText = server_->arg("heartbeat_interval");
  baseUrl.trim();
  reportText.trim();
  heartbeatText.trim();
  while (baseUrl.endsWith("/")) baseUrl.remove(baseUrl.length() - 1);

  uint32_t reportIntervalMs = reportIntervalMs_;
  uint32_t heartbeatIntervalMs = heartbeatIntervalMs_;
  const bool reportValid = reportText.isEmpty()
                               ? true
                               : parseIntervalSeconds(reportText, 1, reportIntervalMs);
  const bool heartbeatValid = heartbeatText.isEmpty()
                                  ? true
                                  : (heartbeatText == "0" || parseIntervalSeconds(heartbeatText, 1, heartbeatIntervalMs));
  if (heartbeatText == "0") heartbeatIntervalMs = 0;
  if (!validBaseUrl(baseUrl) || !validToken(token) || !reportValid || !heartbeatValid) {
    server_->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"invalid reporting configuration\"}");
    return;
  }
  if (!saveConfiguration(baseUrl, token, reportIntervalMs, heartbeatIntervalMs)) {
    server_->send(500, "application/json",
                  "{\"ok\":false,\"error\":\"configuration save failed\"}");
    return;
  }

  reportAttempted_ = false;
  // Let the HTTP response leave the ESP32 before the first potentially blocking upload.
  uploadNotBeforeMs_ = millis() + 1000;
  lastHeartbeatMs_ = millis();
  heartbeatAttempted_ = heartbeatIntervalMs_ > 0;
  lastHttpCode_ = 0;
  timeSyncFailureLogged_ = false;
  Serial.printf("[REPORT] configuration saved: base_url=%s token=%s report=%lus heartbeat=%lus\r\n",
                baseUrl_.c_str(), token_.isEmpty() ? "missing" : "configured",
                static_cast<unsigned long>(reportIntervalMs_ / 1000),
                static_cast<unsigned long>(heartbeatIntervalMs_ / 1000));
  server_->send(200, "application/json", "{\"ok\":true}");
}

void ReportUploader::receiveManualReport() {
  if (server_ == nullptr) return;
  if (baseUrl_.isEmpty()) {
    server_->send(400, "application/json", "{\"ok\":false,\"error\":\"reporting is not configured\"}");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    server_->send(409, "application/json", "{\"ok\":false,\"error\":\"Wi-Fi is not connected\"}");
    return;
  }
  manualRequested_ = true;
  uploadNotBeforeMs_ = millis() + 300;
  server_->send(200, "application/json", "{\"ok\":true,\"queued\":true}");
}

void ReportUploader::appendLog(const char* kind, int code, const String& response, size_t requestBytes) {
  const bool success = successfulHttpCode(code);
  uint8_t successCount = 0;
  int oldestSuccess = -1;
  for (uint8_t i = 0; i < logCount_; ++i) {
    if (successfulHttpCode(logs_[i].code)) {
      if (oldestSuccess < 0) oldestSuccess = i;
      ++successCount;
    }
  }

  auto removeAt = [this](uint8_t index) {
    for (uint8_t i = index; i + 1 < logCount_; ++i) logs_[i] = logs_[i + 1];
    --logCount_;
    logs_[logCount_].kind = String();
    logs_[logCount_].response = String();
  };
  if (success && successCount >= kSuccessLogCapacity) {
    removeAt(static_cast<uint8_t>(oldestSuccess));
  } else if (logCount_ >= kLogCapacity) {
    if (oldestSuccess >= 0) removeAt(static_cast<uint8_t>(oldestSuccess));
    else if (success) return;
    else removeAt(0);
  }

  LogEntry& entry = logs_[logCount_++];
  entry.timeMs = millis();
  entry.code = code;
  entry.kind = kind;
  entry.response = response;
  const size_t responseLimit = success ? kMaxSuccessLogResponseLength : kMaxLogResponseLength;
  if (entry.response.length() > responseLimit) entry.response.remove(responseLimit);
  entry.requestBytes = requestBytes;
}

void ReportUploader::sendLogs() {
  if (server_ == nullptr) return;
  String response = "[";
  for (uint8_t i = 0; i < logCount_; ++i) {
    const LogEntry& entry = logs_[i];
    if (i != 0) response += ',';
    response += "{\"timeMs\":" + String(entry.timeMs) +
                ",\"kind\":\"" + escapeJson(entry.kind) +
                "\",\"code\":" + String(entry.code) +
                ",\"requestBytes\":" + String(entry.requestBytes) +
                ",\"response\":\"" + escapeJson(entry.response) + "\"}";
  }
  response += ']';
  server_->sendHeader("Cache-Control", "no-store");
  server_->send(200, "application/json", response);
}

void ReportUploader::clearLogs() {
  for (uint8_t i = 0; i < logCount_; ++i) {
    logs_[i].kind = String();
    logs_[i].response = String();
  }
  logCount_ = 0;
  server_->send(200, "application/json", "{\"ok\":true}");
}

bool ReportUploader::ensureTlsTime() {
  if (!baseUrl_.startsWith("https://")) return true;
  if (rtcClock.ntpSyncPending()) return false;
  if (systemTimeValid()) {
    if (timeSyncStarted_ && !timeSyncCompleteLogged_) {
      time_t epoch = 0;
      rtcClock.nowEpoch(epoch);
      Serial.printf("[REPORT] TLS clock synchronized: epoch=%lld\r\n",
                    static_cast<long long>(epoch));
      timeSyncCompleteLogged_ = true;
    }
    return true;
  }

  if (!timeSyncStarted_) {
    rtcClock.updateNtp(WiFi.status() == WL_CONNECTED);
    timeSyncStarted_ = true;
    timeSyncStartedMs_ = millis();
    Serial.println("[REPORT] waiting for SNTP time before HTTPS certificate verification");
  }
  if (!timeSyncFailureLogged_ && millis() - timeSyncStartedMs_ >= kTimeSyncWarningMs) {
    lastHttpCode_ = kTransportTimeUnavailable;
    appendLog("tls", lastHttpCode_, "waiting for network time; HTTPS request deferred", 0);
    Serial.println("[REPORT] SNTP time unavailable; HTTPS remains deferred and will retry");
    timeSyncFailureLogged_ = true;
  }
  return false;
}

void ReportUploader::update(const SensorData& data) {
  if (baseUrl_.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  if (static_cast<int32_t>(millis() - uploadNotBeforeMs_) < 0) return;
  if (!ensureTlsTime()) return;
  const uint32_t now = millis();
  const bool heartbeatDue = heartbeatIntervalMs_ > 0 &&
                            (!heartbeatAttempted_ ||
                             now - lastHeartbeatMs_ >= heartbeatIntervalMs_);
  const bool reportDue = !reportAttempted_ ||
                         now - lastReportMs_ >= reportIntervalMs_;

  if (manualRequested_) {
    manualRequested_ = false;
    lastDataUploadMs_ = now;
    hasDataUpload_ = true;
    upload(data, "manual");
    lastReportMs_ = now;
    reportAttempted_ = true;
    if (heartbeatDue) {
      lastHeartbeatMs_ = now;
      lastHeartbeatUploadMs_ = now;
      heartbeatAttempted_ = true;
      hasHeartbeatUpload_ = true;
    }
    return;
  }

  // 心跳也携带完整传感器数据；两种周期同时到期时，一条请求即可同时满足。
  if (heartbeatDue) {
    lastDataUploadMs_ = now;
    lastHeartbeatUploadMs_ = now;
    hasDataUpload_ = true;
    hasHeartbeatUpload_ = true;
    upload(data, "heartbeat");
    lastReportMs_ = now;
    lastHeartbeatMs_ = now;
    reportAttempted_ = true;
    heartbeatAttempted_ = true;
    return;
  }

  if (reportDue) {
    lastDataUploadMs_ = now;
    hasDataUpload_ = true;
    upload(data, "report");
    lastReportMs_ = now;
    reportAttempted_ = true;
  }
}

ReportUploadStatus ReportUploader::status() const {
  ReportUploadStatus result;
  result.hasDataUpload = hasDataUpload_;
  result.lastDataUploadMs = lastDataUploadMs_;
  result.hasHeartbeatUpload = hasHeartbeatUpload_;
  result.lastHeartbeatUploadMs = lastHeartbeatUploadMs_;
  if (baseUrl_.isEmpty()) {
    result.apiConnectivity = ApiConnectivity::NotConfigured;
  } else if (lastHttpCode_ == 0) {
    result.apiConnectivity = ApiConnectivity::Pending;
  } else if (lastHttpCode_ > 0) {
    result.apiConnectivity = ApiConnectivity::Reachable;
  } else {
    result.apiConnectivity = ApiConnectivity::Unreachable;
  }
  return result;
}

String ReportUploader::buildRequest(const SensorData& data, const String& host,
                                    const String& path) const {
  String sensorFields = ",\"light1\":" + String(data.light1Percent, 1) +
                        ",\"light2\":" + String(data.light2Percent, 1) +
                        ",\"light1_adc\":" + String(data.light1Raw) +
                        ",\"light2_adc\":" + String(data.light2Raw);
  if (data.mq135WarmedUp && !isnan(data.mq135AqiEstimate)) {
    sensorFields += ",\"aqi\":" + String(data.mq135AqiEstimate, 0);
  }
  if (data.dhtValid && !isnan(data.temperatureC)) {
    sensorFields += ",\"temperature\":" + String(data.temperatureC, 1);
  }
  if (data.dhtValid && isfinite(data.humidityPercent) &&
      data.humidityPercent >= 0.0f && data.humidityPercent <= 100.0f) {
    sensorFields += ",\"humidity\":" + String(data.humidityPercent, 1);
  }
  sensorFields += ",\"presence\":";
  sensorFields += data.presenceDetected ? "true" : "false";

  const String escapedDeviceId = escapeJson(deviceId_);
  const String rtcTimestamp = rtcClock.isoString();
  String body = "{\"device_id\":\"" + escapedDeviceId + "\"" + sensorFields;
  if (!rtcTimestamp.isEmpty()) body += ",\"timestamp\":\"" + rtcTimestamp + "\"";
  body += "}";
  String request = "POST " + path + " HTTP/1.1\r\n"
                   "Host: " + host + "\r\n";
  if (!token_.isEmpty()) request += "Authorization: Bearer " + token_ + "\r\n";
  request += "Content-Type: application/json; charset=utf-8\r\n"
             "Content-Length: " + String(body.length()) + "\r\n"
             "Connection: close\r\n\r\n" + body;
  return request;
}

int ReportUploader::upload(const SensorData& data, const char* kind) {
  String host;
  String hostHeader;
  String path;
  uint16_t port = 0;
  bool secure = false;
  if (!parseHttpUrl(baseUrl_, host, port, hostHeader, path, secure)) {
    lastHttpCode_ = kTransportInvalidResponse;
    appendLog(kind, lastHttpCode_, "invalid API Base URL", 0);
    return lastHttpCode_;
  }

  const String request = buildRequest(data, hostHeader, path);

  WiFiClient plainClient;
  WiFiClient* client = &plainClient;
  if (secure) {
    static WiFiClientSecure tlsClient;
    static bool tlsConfigured = false;
    if (!tlsConfigured) {
      tlsClient.setCACertBundle(rootca_crt_bundle_start);
      tlsClient.setHandshakeTimeout(5);
      tlsConfigured = true;
    }
    client = &tlsClient;
  }
  if (!client->connect(host.c_str(), port, kConnectTimeoutMs)) {
    lastHttpCode_ = kTransportConnectionFailed;
    appendLog(kind, lastHttpCode_, secure ? "TLS connection or certificate verification failed"
                                         : "connection failed",
              request.length());
    Serial.printf("[REPORT] %s %s connection failed: %s:%u\r\n",
                  kind, secure ? "HTTPS" : "HTTP", host.c_str(),
                  static_cast<unsigned int>(port));
    return lastHttpCode_;
  }
  client->setTimeout(kIoTimeoutMs / 1000);

  if (!writeEntireRequest(*client, request)) {
    lastHttpCode_ = kTransportWriteFailed;
    appendLog(kind, lastHttpCode_, "request write failed", request.length());
    client->stop();
    return lastHttpCode_;
  }

  String response;
  lastHttpCode_ = readHttpResponse(*client, response);
  client->stop();
  appendLog(kind, lastHttpCode_, response, request.length());
  if (lastHttpCode_ == 201) {
    Serial.printf("[REPORT] %s HTTP 201 uploaded bytes=%u\r\n",
                  kind, static_cast<unsigned int>(request.length()));
  } else if (lastHttpCode_ > 0) {
    Serial.printf("[REPORT] %s failed: HTTP %d\r\n", kind, lastHttpCode_);
  } else {
    Serial.printf("[REPORT] %s transport failed: code=%d error=%s\r\n",
                  kind, lastHttpCode_, response.c_str());
  }
  return lastHttpCode_;
}
