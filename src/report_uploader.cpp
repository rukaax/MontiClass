/** @file report_uploader.cpp - 传感器数据的 HTTP/HTTPS 定时上报实现。 */
#include "report_uploader.h"
#include "rtc_clock.h"           // 用来读设备实时时钟、判断 SNTP 是否已同步

#include <Preferences.h>          // ESP32 的 NVS 读写库，用来持久化上报配置
#include <WebServer.h>            // Arduino 的简易 HTTP 服务端库
#include <WiFi.h>                 // WiFi 状态查询与普通 TCP 客户端
#include <WiFiClientSecure.h>     // 支持 TLS 的 WiFi 客户端，用于 https

#include <math.h>                 // isfinite / isnan
#include <time.h>                 // time_t 类型定义

// 由链接脚本注入的根证书包（用于 TLS 校验），位于 data/cert/x509_crt_bundle.bin
extern const uint8_t rootca_crt_bundle_start[]
    asm("_binary_data_cert_x509_crt_bundle_bin_start");

// 匿名命名空间：文件内私有，等价于 C 的 static
namespace {
constexpr char kPreferencesNamespace[] = "report_cfg";        // NVS 命名空间名字
constexpr char kBaseUrlKey[] = "base_url";                    // 保存 Base URL 的键
constexpr char kTokenKey[] = "token";                         // 保存 Token 的键
constexpr char kReportIntervalKey[] = "report_ms";            // 数据上报周期(毫秒)
constexpr char kHeartbeatIntervalKey[] = "heartbeat_ms";      // 心跳周期(毫秒)
constexpr char kObsoleteHeadersKey[] = "headers";             // 旧版本残留键，读到就删
constexpr char kObsoleteRequestTemplateKey[] = "request_tpl"; // 旧版本残留键，读到就删
constexpr size_t kMaxBaseUrlLength = 240;                     // Base URL 允许的最大长度
constexpr size_t kMaxTokenLength = 512;                       // Token 允许的最大长度
constexpr size_t kMaxLogResponseLength = 256;                 // 失败日志里保留的响应片段上限
constexpr size_t kMaxSuccessLogResponseLength = 64;           // 成功日志响应片段更短，省内存
constexpr uint32_t kMinReportIntervalMs = 1000;               // 数据上报最短周期 = 1 秒
constexpr uint32_t kMaxIntervalMs = 86400000UL;               // 周期上限 = 24 小时
constexpr uint32_t kConnectTimeoutMs = 1500;                  // TCP/TLS 连接超时
constexpr uint32_t kIoTimeoutMs = 2000;                       // 单次读写等待超时
constexpr uint32_t kTimeSyncWarningMs = 20000;                // 时钟同步超过 20s 就打一次告警
constexpr time_t kMinimumValidEpoch = 1704067200;             // 2024-01-01 UTC，早于此认为时钟未同步
// 自定义的传输层错误码（用负数以便和真正的 HTTP 状态码区分）
constexpr int kTransportConnectionFailed = -1;                // 连接失败
constexpr int kTransportWriteFailed = -3;                     // 写请求失败
constexpr int kTransportInvalidResponse = -9;                 // 收到的响应无法解析
constexpr int kTransportReadTimeout = -11;                    // 等待响应超时
constexpr int kTransportTimeUnavailable = -13;                // 系统时间未同步，HTTPS 不能发

// 把字符串按 JSON 规则做最小转义（引号/反斜杠/换行/回车），丢弃控制字符。
String escapeJson(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 4);        // 预留一点富余，避免频繁扩容
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '"' || c == '\\') escaped += '\\';   // 引号或反斜杠前加转义
    if (c == '\n') escaped += "\\n";              // 换行 -> \n
    else if (c == '\r') escaped += "\\r";         // 回车 -> \r
    else if (static_cast<uint8_t>(c) >= 0x20) escaped += c;  // 其余可见字符原样输出
  }
  return escaped;
}

// 解析一个 http(s):// URL，拆分出 host / port / hostHeader / path / secure 五个字段。
// 返回 false 表示 URL 非法（长度超限、非法字符、authority 为空等）。
bool parseHttpUrl(const String& value, String& host, uint16_t& port,
                  String& hostHeader, String& path, bool& secure) {
  size_t schemeLength = 0;
  if (value.startsWith("https://")) {          // 判断协议
    secure = true;
    schemeLength = 8;
  } else if (value.startsWith("http://")) {
    secure = false;
    schemeLength = 7;
  } else {
    return false;                              // 只允许 http/https
  }
  // 基础合法性校验：长度、是否含引号/反斜杠/查询串/锚点
  if (value.length() > kMaxBaseUrlLength || value.length() <= schemeLength ||
      value.indexOf('"') >= 0 ||
      value.indexOf('\\') >= 0 || value.indexOf('?') >= 0 ||
      value.indexOf('#') >= 0) return false;
  // 不允许控制字符和空格
  for (size_t i = 0; i < value.length(); ++i) {
    if (static_cast<uint8_t>(value[i]) < 0x20 || value[i] == ' ') return false;
  }

  // 找出 authority 和 path 的分界点（scheme 后第一个 '/'）
  const int pathStart = value.indexOf('/', schemeLength);
  const String authority = pathStart < 0 ? value.substring(schemeLength)
                                         : value.substring(schemeLength, pathStart);
  if (authority.isEmpty() || authority.indexOf('@') >= 0) return false;  // 不接受用户名@
  hostHeader = authority;                      // HTTP Host 头直接用整个 authority
  port = secure ? 443 : 80;                    // 默认端口

  if (authority.startsWith("[")) {
    // 形如 [IPv6]:port 的写法
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
    // 普通 host[:port]
    const int colon = authority.lastIndexOf(':');
    if (colon >= 0) {
      if (authority.indexOf(':') != colon) return false;   // 只允许一个冒号
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

  // 路径：URL 里没写就默认 "/reports"；写了但不以 /reports 结尾就自动补上
  path = pathStart < 0 ? "" : value.substring(pathStart);
  if (path.isEmpty()) path = "/reports";
  else if (!path.endsWith("/reports")) path += "/reports";
  return true;
}

// 简单包装：只用来判断 URL 是否合法，不关心解析结果。
bool validBaseUrl(const String& value) {
  String host;
  String hostHeader;
  String path;
  uint16_t port = 0;
  bool secure = false;
  return parseHttpUrl(value, host, port, hostHeader, path, secure);
}

// 校验 Token：长度不超上限，不含控制字符。
bool validToken(const String& value) {
  if (value.length() > kMaxTokenLength) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (static_cast<uint8_t>(value[i]) < 0x20 || value[i] == 0x7f) return false;
  }
  return true;
}

// 系统时间是否已经通过 SNTP 同步到 RTC。
bool systemTimeValid() { return rtcClock.isValid(); }

// HTTP 2xx 视为成功。
bool successfulHttpCode(int code) {
  return code >= 200 && code < 300;
}

// 把 "10"、"60" 之类的秒数字符串解析成毫秒；不合法返回 false。
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

// 等待 client 有数据可读；超时或断链返回 false。
bool waitForResponseData(WiFiClient& client, uint32_t timeoutMs) {
  const uint32_t started = millis();
  while (client.available() == 0) {
    if (!client.connected() || millis() - started >= timeoutMs) return false;
    delay(1);                                  // 让出 CPU，避免忙等
  }
  return true;
}

// 把整段 request 写完；期间失败或长时间没进展就返回 false。
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
      return false;                            // 太久没写出去，判断为失败
    } else {
      delay(1);
    }
  }
  return true;
}

// 手工解析 HTTP 响应：读状态行 -> 读头 -> 按 Content-Length 读响应体。
// 返回值：正数=HTTP 状态码，负数=自定义传输错误码。
int readHttpResponse(WiFiClient& client, String& response) {
  if (!waitForResponseData(client, kIoTimeoutMs)) {
    response = "response timeout";
    return kTransportReadTimeout;
  }

  // ---- 状态行，例如 "HTTP/1.1 201 Created" ----
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

  // ---- 响应头：只关心 Content-Length，其余忽略 ----
  int contentLength = -1;                      // -1 表示未知，读到空行为止
  while (true) {
    if (!waitForResponseData(client, kIoTimeoutMs)) {
      response = "response header timeout";
      return statusCode;
    }
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) break;                 // 空行 = 头结束
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      const String lengthText = line.substring(line.indexOf(':') + 1);
      contentLength = lengthText.toInt();
      if (contentLength < 0) contentLength = -1;
    }
  }

  // ---- 响应体：按 Content-Length 读满；若未知就读到断链/超时 ----
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

// ========== ReportUploader 类的实现 ==========

// 上电初始化：生成 deviceId、加载 NVS 配置，打印开机日志。
void ReportUploader::begin() {
  char deviceId[32];
  // 用芯片 eFuse MAC 的低 48 位拼一个稳定的设备 ID
  const uint64_t chipId = ESP.getEfuseMac() & 0x0000FFFFFFFFFFFFULL;
  snprintf(deviceId, sizeof(deviceId), "esp32s3-%012llX",
           static_cast<unsigned long long>(chipId));
  deviceId_ = deviceId;
  loadConfiguration();                         // 从 NVS 恢复上次的配置
  lastHeartbeatMs_ = millis();
  heartbeatAttempted_ = heartbeatIntervalMs_ > 0;  // 心跳未启用时，直接标记为“无需尝试”
  Serial.printf("[REPORT] device_id=%s configured=%s report=%lus heartbeat=%lus\r\n",
                deviceId_.c_str(), !baseUrl_.isEmpty() ? "yes" : "no",
                static_cast<unsigned long>(reportIntervalMs_ / 1000),
                static_cast<unsigned long>(heartbeatIntervalMs_ / 1000));
}

// 挂载 /api/reporting/* 到 WebServer；捕获 server 指针以便后续构造响应。
void ReportUploader::configureRoutes(WebServer& server) {
  server_ = &server;
  server.on("/api/reporting", HTTP_GET, [this]() { sendConfiguration(); });
  server.on("/api/reporting", HTTP_POST, [this]() { receiveConfiguration(); });
  server.on("/api/reporting/send", HTTP_POST, [this]() { receiveManualReport(); });
  server.on("/api/reporting/logs", HTTP_GET, [this]() { sendLogs(); });
  server.on("/api/reporting/logs/clear", HTTP_POST, [this]() { clearLogs(); });
}

// 从 NVS 读取配置，同时清理旧版本残留键。返回是否已经配置好 baseUrl。
bool ReportUploader::loadConfiguration() {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;  // false = 读写模式
  baseUrl_ = preferences.getString(kBaseUrlKey, "");
  token_ = preferences.getString(kTokenKey, "");
  reportIntervalMs_ = preferences.getULong(kReportIntervalKey, kDefaultReportIntervalMs);
  heartbeatIntervalMs_ = preferences.getULong(kHeartbeatIntervalKey, kDefaultHeartbeatIntervalMs);
  if (preferences.isKey(kObsoleteHeadersKey)) preferences.remove(kObsoleteHeadersKey);
  if (preferences.isKey(kObsoleteRequestTemplateKey)) {
    preferences.remove(kObsoleteRequestTemplateKey);
  }
  preferences.end();
  // 越界或异常值 -> 回落到默认
  if (reportIntervalMs_ < kMinReportIntervalMs || reportIntervalMs_ > kMaxIntervalMs) {
    reportIntervalMs_ = kDefaultReportIntervalMs;
  }
  if (heartbeatIntervalMs_ > kMaxIntervalMs) heartbeatIntervalMs_ = kDefaultHeartbeatIntervalMs;
  return !baseUrl_.isEmpty();
}

// 保存配置到 NVS，成功后同步更新内存里的成员。
bool ReportUploader::saveConfiguration(const String& baseUrl, const String& token,
                                       uint32_t reportIntervalMs, uint32_t heartbeatIntervalMs) {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;
  // putString 返回写入的字节数；等于 length 就算成功
  const bool baseSaved = preferences.putString(kBaseUrlKey, baseUrl) == baseUrl.length();
  bool tokenSaved = true;
  // 空 token 视为“不改”，保留原来的
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

// GET /api/reporting - 把当前配置和运行状态以 JSON 形式返给前端。
void ReportUploader::sendConfiguration() {
  if (server_ == nullptr) return;
  const bool configured = !baseUrl_.isEmpty();
  const ReportUploadStatus uploadStatus = status();
  // 把枚举翻译成前端好读的字符串
  const char* apiConnectivity = "pending";
  if (uploadStatus.apiConnectivity == ApiConnectivity::NotConfigured) {
    apiConnectivity = "not_configured";
  } else if (uploadStatus.apiConnectivity == ApiConnectivity::Reachable) {
    apiConnectivity = "reachable";
  } else if (uploadStatus.apiConnectivity == ApiConnectivity::Unreachable) {
    apiConnectivity = "unreachable";
  }
  // 手工拼 JSON：字段较少，值都是内部生成的，无需第三方 JSON 库
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
                    ",\"lastDataUploadMs\":" +
                    String(uploadStatus.lastDataUploadMs) +
                    ",\"hasHeartbeatUpload\":" +
                    (uploadStatus.hasHeartbeatUpload ? "true" : "false") +
                    ",\"lastHeartbeatUploadMs\":" +
                    String(uploadStatus.lastHeartbeatUploadMs) +
                    ",\"apiConnectivity\":\"" + apiConnectivity + "\"" +
                    ",\"lastHttpCode\":" + String(lastHttpCode_) + "}";
  server_->sendHeader("Cache-Control", "no-store");
  server_->send(200, "application/json", response);
}

// POST /api/reporting - 接收 WebUI 提交的新配置。
void ReportUploader::receiveConfiguration() {
  if (server_ == nullptr) return;
  String baseUrl = server_->arg("base_url");
  const String token = server_->arg("token");
  String reportText = server_->arg("report_interval");
  String heartbeatText = server_->arg("heartbeat_interval");
  baseUrl.trim();
  reportText.trim();
  heartbeatText.trim();
  while (baseUrl.endsWith("/")) baseUrl.remove(baseUrl.length() - 1);  // 去掉结尾多余的斜杠

  uint32_t reportIntervalMs = reportIntervalMs_;
  uint32_t heartbeatIntervalMs = heartbeatIntervalMs_;
  // 上报周期：留空表示不改，否则必须能解析成秒
  const bool reportValid = reportText.isEmpty()
                               ? true
                               : parseIntervalSeconds(reportText, 1, reportIntervalMs);
  // 心跳：留空=不改；"0"=关闭；其它必须能解析
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
  // 给 HTTP 响应一点时间先返回，再触发可能会阻塞的上传
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

// POST /api/reporting/send - 用户点“立即上报”，标记一下等主循环执行。
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
  uploadNotBeforeMs_ = millis() + 300;         // 让本次 HTTP 响应先返回
  server_->send(200, "application/json", "{\"ok\":true,\"queued\":true}");
}

// 往环形日志里追加一条；策略：成功日志和失败日志分别限流，避免刷屏。
void ReportUploader::appendLog(const char* kind, int code, const String& response, size_t requestBytes) {
  const bool success = successfulHttpCode(code);
  uint8_t successCount = 0;
  int oldestSuccess = -1;
  // 数一下当前有几条成功日志，同时找到最旧的一条
  for (uint8_t i = 0; i < logCount_; ++i) {
    if (successfulHttpCode(logs_[i].code)) {
      if (oldestSuccess < 0) oldestSuccess = i;
      ++successCount;
    }
  }

  // 从数组里删除某一项，后面的往前挪
  auto removeAt = [this](uint8_t index) {
    for (uint8_t i = index; i + 1 < logCount_; ++i) logs_[i] = logs_[i + 1];
    --logCount_;
    logs_[logCount_].kind = String();          // 显式清空，释放 String 里的堆
    logs_[logCount_].response = String();
  };
  if (success && successCount >= kSuccessLogCapacity) {
    // 成功日志超过配额，先淘汰最旧的成功日志
    removeAt(static_cast<uint8_t>(oldestSuccess));
  } else if (logCount_ >= kLogCapacity) {
    // 总容量满了：优先淘汰最旧的成功日志，其次淘汰最旧的失败日志；新条目是成功就直接丢
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

// GET /api/reporting/logs - 把内存里的日志序列化成 JSON 数组返回。
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

// POST /api/reporting/logs/clear - 清空日志缓冲。
void ReportUploader::clearLogs() {
  for (uint8_t i = 0; i < logCount_; ++i) {
    logs_[i].kind = String();
    logs_[i].response = String();
  }
  logCount_ = 0;
  server_->send(200, "application/json", "{\"ok\":true}");
}

// HTTPS 之前必须先有可信的系统时间，否则证书里的“有效期”没法验证。
// 返回 true 表示可以发；返回 false 表示还要再等（本次 update 跳过）。
bool ReportUploader::ensureTlsTime() {
  if (!baseUrl_.startsWith("https://")) return true;   // 纯 HTTP 无所谓
  if (rtcClock.ntpSyncPending()) return false;         // 正在同步，先等
  if (systemTimeValid()) {
    // 首次同步完成时打一条日志，之后就不再重复了
    if (timeSyncStarted_ && !timeSyncCompleteLogged_) {
      time_t epoch = 0;
      rtcClock.nowEpoch(epoch);
      Serial.printf("[REPORT] TLS clock synchronized: epoch=%lld\r\n",
                    static_cast<long long>(epoch));
      timeSyncCompleteLogged_ = true;
    }
    return true;
  }

  // 还没同步过就主动触发一次
  if (!timeSyncStarted_) {
    rtcClock.updateNtp(WiFi.status() == WL_CONNECTED);
    timeSyncStarted_ = true;
    timeSyncStartedMs_ = millis();
    Serial.println("[REPORT] waiting for SNTP time before HTTPS certificate verification");
  }
  // 等太久了，记录一次告警（后续不再重复）
  if (!timeSyncFailureLogged_ && millis() - timeSyncStartedMs_ >= kTimeSyncWarningMs) {
    lastHttpCode_ = kTransportTimeUnavailable;
    appendLog("tls", lastHttpCode_, "waiting for network time; HTTPS request deferred", 0);
    Serial.println("[REPORT] SNTP time unavailable; HTTPS remains deferred and will retry");
    timeSyncFailureLogged_ = true;
  }
  return false;
}

// 主循环调用：判断是不是到点了，如果到点就发一次上报。
void ReportUploader::update(const SensorData& data) {
  if (baseUrl_.isEmpty() || WiFi.status() != WL_CONNECTED) return;   // 未配置或没网就不做
  // 若配置刚更新，会有一个 uploadNotBeforeMs_ 时间点：在此之前不发送
  if (static_cast<int32_t>(millis() - uploadNotBeforeMs_) < 0) return;
  if (!ensureTlsTime()) return;
  const uint32_t now = millis();
  const bool heartbeatDue = heartbeatIntervalMs_ > 0 &&
                            (!heartbeatAttempted_ ||
                             now - lastHeartbeatMs_ >= heartbeatIntervalMs_);
  const bool reportDue = !reportAttempted_ ||
                         now - lastReportMs_ >= reportIntervalMs_;

  // 手动触发优先：占用一次数据上报的“配额”，若心跳也到期就一并算完
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

// 供 OLED/WebUI 查询：把内部计数、时间戳、可达性打包成状态快照
ReportUploadStatus ReportUploader::status() const {
  ReportUploadStatus result;
  result.hasDataUpload = hasDataUpload_;
  result.lastDataUploadMs = lastDataUploadMs_;
  result.hasHeartbeatUpload = hasHeartbeatUpload_;
  result.lastHeartbeatUploadMs = lastHeartbeatUploadMs_;
  if (baseUrl_.isEmpty()) {
    result.apiConnectivity = ApiConnectivity::NotConfigured;
  } else if (lastHttpCode_ == 0) {
    result.apiConnectivity = ApiConnectivity::Pending;      // 还没跑过
  } else if (lastHttpCode_ > 0) {
    result.apiConnectivity = ApiConnectivity::Reachable;    // 收到了 HTTP 状态码就算可达
  } else {
    result.apiConnectivity = ApiConnectivity::Unreachable;  // 负数=传输失败
  }
  return result;
}

// 生成一个完整的 HTTP 请求文本（POST 请求行 + 头 + JSON body）。
String ReportUploader::buildRequest(const SensorData& data, const String& host,
                                    const String& path) const {
  // 传感器字段：光敏一直有，其他字段按有效性挑选
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
  // 手工拼 HTTP：用 Connection: close，读完响应体就关闭连接
  String request = "POST " + path + " HTTP/1.1\r\n"
                   "Host: " + host + "\r\n";
  if (!token_.isEmpty()) request += "Authorization: Bearer " + token_ + "\r\n";
  request += "Content-Type: application/json; charset=utf-8\r\n"
             "Content-Length: " + String(body.length()) + "\r\n"
             "Connection: close\r\n\r\n" + body;
  return request;
}

// 实际执行一次 HTTP(S) 上传，返回 HTTP 状态码或负数错误码；同时更新日志。
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

  // 根据 http/https 选择底层客户端。TLS 客户端做成 static 以复用握手证书缓存。
  WiFiClient plainClient;
  WiFiClient* client = &plainClient;
  if (secure) {
    static WiFiClientSecure tlsClient;
    static bool tlsConfigured = false;
    if (!tlsConfigured) {
      tlsClient.setCACertBundle(rootca_crt_bundle_start);   // 用内置根证书包
      tlsClient.setHandshakeTimeout(5);                     // 单位：秒
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
  client->setTimeout(kIoTimeoutMs / 1000);      // Arduino API 用秒

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
