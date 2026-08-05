/** @file report_uploader.cpp */
#include "report_uploader.h"

#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include <ctype.h>
#include <math.h>

namespace {
constexpr char kPreferencesNamespace[] = "report_cfg";
constexpr char kBaseUrlKey[] = "base_url";
constexpr char kTokenKey[] = "token";
constexpr char kReportIntervalKey[] = "report_ms";
constexpr char kHeartbeatIntervalKey[] = "heartbeat_ms";
constexpr char kHeadersKey[] = "headers";
constexpr size_t kMaxBaseUrlLength = 240;
constexpr size_t kMaxTokenLength = 512;
constexpr size_t kMaxHeadersLength = 1024;
constexpr size_t kMaxLogResponseLength = 256;
constexpr uint32_t kMinReportIntervalMs = 1000;
constexpr uint32_t kMaxIntervalMs = 86400000UL;

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

bool validBaseUrl(const String& value) {
  if (!value.startsWith("http://") || value.length() > kMaxBaseUrlLength) return false;
  if (value.length() <= 7 || value.indexOf('"') >= 0 || value.indexOf('\\') >= 0) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (static_cast<uint8_t>(value[i]) < 0x20 || value[i] == ' ') return false;
  }
  return true;
}

bool reservedHeader(const String& name) {
  String lower = name;
  lower.toLowerCase();
  return lower == "authorization" || lower == "content-type" ||
         lower == "content-length" || lower == "host";
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
                deviceId_.c_str(), (!baseUrl_.isEmpty() && !token_.isEmpty()) ? "yes" : "no",
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
  if (!preferences.begin(kPreferencesNamespace, true)) return false;
  baseUrl_ = preferences.getString(kBaseUrlKey, "");
  token_ = preferences.getString(kTokenKey, "");
  reportIntervalMs_ = preferences.getULong(kReportIntervalKey, kDefaultReportIntervalMs);
  heartbeatIntervalMs_ = preferences.getULong(kHeartbeatIntervalKey, kDefaultHeartbeatIntervalMs);
  customHeaders_ = preferences.getString(kHeadersKey, "");
  preferences.end();
  if (reportIntervalMs_ < kMinReportIntervalMs || reportIntervalMs_ > kMaxIntervalMs) {
    reportIntervalMs_ = kDefaultReportIntervalMs;
  }
  if (heartbeatIntervalMs_ > kMaxIntervalMs) heartbeatIntervalMs_ = kDefaultHeartbeatIntervalMs;
  if (!validateCustomHeaders(customHeaders_)) customHeaders_ = "";
  return !baseUrl_.isEmpty() && !token_.isEmpty();
}

bool ReportUploader::saveConfiguration(const String& baseUrl, const String& token,
                                       uint32_t reportIntervalMs, uint32_t heartbeatIntervalMs,
                                       const String& customHeaders) {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;
  const bool baseSaved = preferences.putString(kBaseUrlKey, baseUrl) == baseUrl.length();
  bool tokenSaved = true;
  if (!token.isEmpty()) tokenSaved = preferences.putString(kTokenKey, token) == token.length();
  const bool reportSaved = preferences.putULong(kReportIntervalKey, reportIntervalMs) != 0;
  const bool heartbeatSaved = preferences.putULong(kHeartbeatIntervalKey, heartbeatIntervalMs) != 0;
  const bool headersSaved = preferences.putString(kHeadersKey, customHeaders) == customHeaders.length();
  preferences.end();
  if (!baseSaved || !tokenSaved || !reportSaved || !heartbeatSaved || !headersSaved) return false;
  baseUrl_ = baseUrl;
  if (!token.isEmpty()) token_ = token;
  reportIntervalMs_ = reportIntervalMs;
  heartbeatIntervalMs_ = heartbeatIntervalMs;
  customHeaders_ = customHeaders;
  return !token_.isEmpty();
}

void ReportUploader::sendConfiguration() {
  if (server_ == nullptr) return;
  const bool configured = !baseUrl_.isEmpty() && !token_.isEmpty();
  String response = "{\"deviceId\":\"" + escapeJson(deviceId_) +
                    "\",\"baseUrl\":\"" + escapeJson(baseUrl_) +
                    "\",\"tokenConfigured\":" + (token_.isEmpty() ? "false" : "true") +
                    ",\"configured\":" + (configured ? "true" : "false") +
                    ",\"reportIntervalSec\":" + String(reportIntervalMs_ / 1000) +
                    ",\"heartbeatIntervalSec\":" + String(heartbeatIntervalMs_ / 1000) +
                    ",\"customHeaders\":\"" + escapeJson(customHeaders_) +
                    "\",\"lastHttpCode\":" + String(lastHttpCode_) + "}";
  server_->sendHeader("Cache-Control", "no-store");
  server_->send(200, "application/json", response);
}

bool ReportUploader::validateCustomHeaders(const String& headers) const {
  if (headers.length() > kMaxHeadersLength) return false;
  size_t start = 0;
  while (start < headers.length()) {
    int end = headers.indexOf('\n', start);
    if (end < 0) end = headers.length();
    String line = headers.substring(start, end);
    line.trim();
    if (!line.isEmpty()) {
      const int colon = line.indexOf(':');
      if (colon <= 0) return false;
      String name = line.substring(0, colon);
      String value = line.substring(colon + 1);
      name.trim();
      value.trim();
      if (name.isEmpty() || value.indexOf('\r') >= 0 || value.indexOf('\n') >= 0 || reservedHeader(name)) {
        return false;
      }
      for (size_t i = 0; i < name.length(); ++i) {
        const char c = name[i];
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '-')) return false;
      }
      for (size_t i = 0; i < value.length(); ++i) {
        if (static_cast<uint8_t>(value[i]) < 0x20) return false;
      }
    }
    start = static_cast<size_t>(end) + 1;
  }
  return true;
}

void ReportUploader::applyCustomHeaders(HTTPClient& http) const {
  size_t start = 0;
  while (start < customHeaders_.length()) {
    int end = customHeaders_.indexOf('\n', start);
    if (end < 0) end = customHeaders_.length();
    String line = customHeaders_.substring(start, end);
    line.trim();
    if (!line.isEmpty()) {
      const int colon = line.indexOf(':');
      if (colon > 0) {
        String name = line.substring(0, colon);
        String value = line.substring(colon + 1);
        name.trim();
        value.trim();
        http.addHeader(name, value);
      }
    }
    start = static_cast<size_t>(end) + 1;
  }
}

void ReportUploader::receiveConfiguration() {
  if (server_ == nullptr) return;
  String baseUrl = server_->arg("base_url");
  const String token = server_->arg("token");
  String reportText = server_->arg("report_interval");
  String heartbeatText = server_->arg("heartbeat_interval");
  String customHeaders = server_->arg("headers");
  baseUrl.trim();
  reportText.trim();
  heartbeatText.trim();
  customHeaders.trim();
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
  if (!validBaseUrl(baseUrl) || token.length() > kMaxTokenLength ||
      (token.isEmpty() && token_.isEmpty()) || !reportValid || !heartbeatValid ||
      !validateCustomHeaders(customHeaders)) {
    server_->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"invalid reporting configuration\"}");
    return;
  }
  if (!saveConfiguration(baseUrl, token, reportIntervalMs, heartbeatIntervalMs, customHeaders)) {
    server_->send(500, "application/json",
                  "{\"ok\":false,\"error\":\"configuration save failed\"}");
    return;
  }

  reportAttempted_ = false;
  lastHeartbeatMs_ = millis();
  heartbeatAttempted_ = heartbeatIntervalMs_ > 0;
  lastHttpCode_ = 0;
  Serial.printf("[REPORT] configuration saved: base_url=%s token=%s report=%lus heartbeat=%lus headers=%u\r\n",
                baseUrl_.c_str(), token_.isEmpty() ? "missing" : "configured",
                static_cast<unsigned long>(reportIntervalMs_ / 1000),
                static_cast<unsigned long>(heartbeatIntervalMs_ / 1000),
                static_cast<unsigned int>(customHeaders_.length()));
  server_->send(200, "application/json", "{\"ok\":true}");
}

void ReportUploader::receiveManualReport() {
  if (server_ == nullptr) return;
  if (baseUrl_.isEmpty() || token_.isEmpty()) {
    server_->send(400, "application/json", "{\"ok\":false,\"error\":\"reporting is not configured\"}");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    server_->send(409, "application/json", "{\"ok\":false,\"error\":\"Wi-Fi is not connected\"}");
    return;
  }
  manualRequested_ = true;
  server_->send(200, "application/json", "{\"ok\":true,\"queued\":true}");
}

void ReportUploader::appendLog(const char* kind, int code, const String& response, size_t requestBytes) {
  LogEntry& entry = logs_[logNext_];
  entry.timeMs = millis();
  entry.code = code;
  entry.kind = kind;
  entry.response = response;
  if (entry.response.length() > kMaxLogResponseLength) entry.response.remove(kMaxLogResponseLength);
  entry.requestBytes = requestBytes;
  logNext_ = (logNext_ + 1) % kLogCapacity;
  if (logCount_ < kLogCapacity) ++logCount_;
}

void ReportUploader::sendLogs() {
  if (server_ == nullptr) return;
  String response = "[";
  for (uint8_t i = 0; i < logCount_; ++i) {
    const uint8_t index = (logNext_ + kLogCapacity - logCount_ + i) % kLogCapacity;
    const LogEntry& entry = logs_[index];
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
  logCount_ = 0;
  logNext_ = 0;
  server_->send(200, "application/json", "{\"ok\":true}");
}

String ReportUploader::reportsUrl() const {
  return baseUrl_.endsWith("/reports") ? baseUrl_ : baseUrl_ + "/reports";
}

void ReportUploader::update(const SensorData& data) {
  if (baseUrl_.isEmpty() || token_.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();
  bool dataUploaded = false;
  if (manualRequested_) {
    manualRequested_ = false;
    upload(data, "manual", true);
    lastReportMs_ = now;
    reportAttempted_ = true;
    dataUploaded = true;
  }
  if (!dataUploaded && (!reportAttempted_ || now - lastReportMs_ >= reportIntervalMs_)) {
    upload(data, "report", true);
    lastReportMs_ = now;
    reportAttempted_ = true;
    dataUploaded = true;
  }
  if (heartbeatIntervalMs_ > 0 &&
      (!heartbeatAttempted_ || now - lastHeartbeatMs_ >= heartbeatIntervalMs_)) {
    SensorData heartbeatData{};
    upload(heartbeatData, "heartbeat", false);
    lastHeartbeatMs_ = now;
    heartbeatAttempted_ = true;
  }
}

int ReportUploader::upload(const SensorData& data, const char* kind, bool includeSensorData) {
  String body = "{\"device_id\":\"" + escapeJson(deviceId_) + "\"";
  if (includeSensorData) {
    body += ",\"light1\":" + String(data.light1Percent, 1) +
            ",\"light2\":" + String(data.light2Percent, 1) +
            ",\"light1_adc\":" + String(data.light1Raw) +
            ",\"light2_adc\":" + String(data.light2Raw);
    if (data.mq135WarmedUp && !isnan(data.mq135AqiEstimate)) {
      body += ",\"aqi\":" + String(data.mq135AqiEstimate, 0);
    }
    if (data.dhtValid && !isnan(data.temperatureC)) {
      body += ",\"temperature\":" + String(data.temperatureC, 1);
    }
    body += ",\"presence\":";
    body += data.presenceDetected ? "true" : "false";
  }
  body += '}';

  WiFiClient client;
  HTTPClient http;
  const String url = reportsUrl();
  if (!http.begin(client, url)) {
    lastHttpCode_ = HTTPC_ERROR_NO_HTTP_SERVER;
    appendLog(kind, lastHttpCode_, "request setup failed", body.length());
    Serial.printf("[REPORT] request setup failed: %s\r\n", url.c_str());
    return lastHttpCode_;
  }
  http.setConnectTimeout(1500);
  http.setTimeout(2000);
  applyCustomHeaders(http);
  http.addHeader("Authorization", "Bearer " + token_);
  http.addHeader("Content-Type", "application/json; charset=utf-8");
  lastHttpCode_ = http.POST(body);
  String response = lastHttpCode_ > 0 ? http.getString() : HTTPClient::errorToString(lastHttpCode_);
  appendLog(kind, lastHttpCode_, response, body.length());
  if (lastHttpCode_ == HTTP_CODE_CREATED) {
    Serial.printf("[REPORT] %s HTTP 201 uploaded bytes=%u\r\n",
                  kind, static_cast<unsigned int>(body.length()));
  } else if (lastHttpCode_ > 0) {
    Serial.printf("[REPORT] %s failed: HTTP %d\r\n", kind, lastHttpCode_);
  } else {
    Serial.printf("[REPORT] %s transport failed: code=%d error=%s\r\n",
                  kind, lastHttpCode_, response.c_str());
  }
  http.end();
  return lastHttpCode_;
}
