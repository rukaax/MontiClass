/** @file local_https.cpp
 * @brief HTTPS endpoints for calls made by a public HTTPS page to the ESP32 LAN address.
 */
#include "local_https.h"

#include <Preferences.h>
#include <esp_https_server.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

#include "buzzer.h"
#include "fan.h"
#include "lighting.h"
#include "rtc_clock.h"
#include "servo.h"

namespace {
constexpr char kTlsNamespace[] = "local_tls";
constexpr char kCaKeyKey[] = "ca_key";
constexpr char kCaCertKey[] = "ca_cert";
constexpr char kCaName[] = "CN=Classroom ESP32 Local CA,O=MontiClass";
constexpr char kServerName[] = "CN=classroom-esp32.local,O=MontiClass";
constexpr char kNotBefore[] = "20240101000000";
constexpr char kNotAfter[] = "20491231235959";
constexpr uint32_t kStartRetryMs = 30000;

String jsonNumber(float value, uint8_t digits) {
  return isnan(value) ? "null" : String(value, static_cast<unsigned int>(digits));
}

void addBrowserHeaders(httpd_req_t* request) {
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Headers", "Content-Type, Authorization");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Private-Network", "true");
  httpd_resp_set_hdr(request, "Cross-Origin-Resource-Policy", "cross-origin");
}

esp_err_t sendResponse(httpd_req_t* request, int status, const char* type, const String& body) {
  char statusText[32];
  snprintf(statusText, sizeof(statusText), "%d %s", status,
           status == 200 ? "OK" : status == 204 ? "No Content" :
           status == 400 ? "Bad Request" : status == 503 ? "Service Unavailable" : "Error");
  httpd_resp_set_status(request, statusText);
  httpd_resp_set_type(request, type);
  addBrowserHeaders(request);
  return httpd_resp_send(request, body.c_str(), body.length());
}

String urlDecode(const String& encoded) {
  String decoded;
  decoded.reserve(encoded.length());
  for (size_t i = 0; i < encoded.length(); ++i) {
    const char c = encoded[i];
    if (c == '+') {
      decoded += ' ';
    } else if (c == '%' && i + 2 < encoded.length()) {
      char hex[3] = {encoded[i + 1], encoded[i + 2], 0};
      char* end = nullptr;
      const long value = strtol(hex, &end, 16);
      if (end == hex + 2) {
        decoded += static_cast<char>(value);
        i += 2;
      } else {
        decoded += c;
      }
    } else {
      decoded += c;
    }
  }
  return decoded;
}

bool formValue(const String& body, const char* key, String& value) {
  const String prefix = String(key) + '=';
  size_t start = 0;
  while (start <= body.length()) {
    int end = body.indexOf('&', start);
    if (end < 0) end = body.length();
    const String item = body.substring(start, static_cast<size_t>(end));
    if (item.startsWith(prefix)) {
      value = urlDecode(item.substring(prefix.length()));
      return true;
    }
    start = static_cast<size_t>(end) + 1;
  }
  return false;
}

bool readBody(httpd_req_t* request, String& body) {
  if (request->content_len <= 0 || request->content_len > 512) return false;
  body.reserve(request->content_len);
  char buffer[129];
  int remaining = request->content_len;
  while (remaining > 0) {
    const int chunk = remaining < 128 ? remaining : 128;
    const int received = httpd_req_recv(request, buffer, chunk);
    if (received <= 0) return false;
    buffer[received] = 0;
    body.concat(buffer, static_cast<unsigned int>(received));
    remaining -= received;
  }
  return true;
}

bool initEcKey(mbedtls_pk_context& key, mbedtls_ctr_drbg_context& random) {
  mbedtls_pk_init(&key);
  if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) return false;
  return mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                             mbedtls_ctr_drbg_random, &random) == 0;
}

bool writePrivateKey(mbedtls_pk_context& key, String& output) {
  unsigned char pem[512] = {};
  if (mbedtls_pk_write_key_pem(&key, pem, sizeof(pem)) != 0) return false;
  output = reinterpret_cast<const char*>(pem);
  return !output.isEmpty();
}

bool writeCertificate(mbedtls_x509write_cert& certificate, mbedtls_ctr_drbg_context& random,
                      String& output) {
  unsigned char pem[2048] = {};
  if (mbedtls_x509write_crt_pem(&certificate, pem, sizeof(pem),
                                mbedtls_ctr_drbg_random, &random) != 0) return false;
  output = reinterpret_cast<const char*>(pem);
  return !output.isEmpty();
}

bool seedRandom(mbedtls_entropy_context& entropy, mbedtls_ctr_drbg_context& random) {
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&random);
  constexpr char personalization[] = "classroom-esp32-local-tls";
  return mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy,
                               reinterpret_cast<const unsigned char*>(personalization),
                               sizeof(personalization) - 1) == 0;
}

bool setCommonCertificateFields(mbedtls_x509write_cert& certificate,
                                mbedtls_pk_context& subjectKey,
                                mbedtls_pk_context& issuerKey,
                                const char* subjectName, const char* issuerName,
                                uint32_t serialNumber) {
  mbedtls_mpi serial;
  mbedtls_mpi_init(&serial);
  const int serialResult = mbedtls_mpi_lset(&serial, serialNumber);
  mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&certificate, &subjectKey);
  mbedtls_x509write_crt_set_issuer_key(&certificate, &issuerKey);
  const bool ok = serialResult == 0 &&
      mbedtls_x509write_crt_set_serial(&certificate, &serial) == 0 &&
      mbedtls_x509write_crt_set_subject_name(&certificate, subjectName) == 0 &&
      mbedtls_x509write_crt_set_issuer_name(&certificate, issuerName) == 0 &&
      mbedtls_x509write_crt_set_validity(&certificate, kNotBefore, kNotAfter) == 0;
  mbedtls_mpi_free(&serial);
  return ok;
}

bool addServerSan(mbedtls_x509write_cert& certificate, const IPAddress& ip) {
  unsigned char value[8] = {};
  size_t offset = 0;
  value[offset++] = 0x30;
  value[offset++] = 6;
  value[offset++] = 0x87;
  value[offset++] = 4;
  for (uint8_t i = 0; i < 4; ++i) value[offset++] = ip[i];
  return mbedtls_x509write_crt_set_extension(
             &certificate, MBEDTLS_OID_SUBJECT_ALT_NAME,
             MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, value, offset) == 0;
}

bool addServerExtendedKeyUsage(mbedtls_x509write_cert& certificate) {
  static const unsigned char value[] = {
      0x30, 0x0A, 0x06, 0x08, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01};
  return mbedtls_x509write_crt_set_extension(
             &certificate, MBEDTLS_OID_EXTENDED_KEY_USAGE,
             MBEDTLS_OID_SIZE(MBEDTLS_OID_EXTENDED_KEY_USAGE), 0,
             value, sizeof(value)) == 0;
}

LocalHttpsApi* apiFrom(httpd_req_t* request) {
  return static_cast<LocalHttpsApi*>(request->user_ctx);
}

esp_err_t optionsHandler(httpd_req_t* request) {
  return sendResponse(request, 204, "text/plain", "");
}

esp_err_t statusHandler(httpd_req_t* request) {
  LocalHttpsApi* api = apiFrom(request);
  const SensorData data = api->latestData();
  const String rtcNow = rtcClock.isoString();
  const String response = "{\"light\":" + jsonNumber(data.light1Percent, 0) +
      ",\"light2\":" + jsonNumber(data.light2Percent, 0) +
      ",\"air\":" + (data.mq135WarmedUp ? jsonNumber(data.mq135AqiEstimate, 0) : "null") +
      ",\"airRaw\":" + String(data.mq135Raw) +
      ",\"airMv\":" + String(data.mq135MilliVolts) +
      ",\"airBaseline\":" + String(data.mq135BaselineRaw) +
      ",\"airReady\":" + (data.mq135WarmedUp ? "true" : "false") +
      ",\"temperature\":" + (data.dhtValid ? jsonNumber(data.temperatureC, 1) : "null") +
      ",\"humidity\":" + (data.dhtValid ? jsonNumber(data.humidityPercent, 0) : "null") +
      ",\"presence\":" + (data.presenceDetected ? "true" : "false") +
      ",\"connected\":true,\"rtcNow\":\"" + rtcNow +
      "\",\"rtcValid\":" + (rtcNow.isEmpty() ? "false" : "true") +
      ",\"ip\":\"" + api->httpsBaseUrl().substring(8) + "\"}";
  return sendResponse(request, 200, "application/json", response);
}

String actuatorJson() {
  char response[352];
  snprintf(response, sizeof(response),
           "{\"ready\":%s,\"angle\":%u,\"manual\":%s,\"fanReady\":%s,\"fanRunning\":%s,\"fanManual\":%s,\"fanDirection\":%d,\"fanSpeed\":%u,\"lightingReady\":%s,\"lightingManual\":%s,\"led1Brightness\":%u,\"led2Brightness\":%u,\"led3Brightness\":%u,\"buzzerEnabled\":%s}",
           servoReady() ? "true" : "false", static_cast<unsigned>(servoCurrentAngle()),
           servoManualMode() ? "true" : "false", fanReady() ? "true" : "false",
           fanRunning() ? "true" : "false", fanManualMode() ? "true" : "false",
           static_cast<int>(fanDirection()), static_cast<unsigned>(fanSpeedPercent()),
           lightingReady() ? "true" : "false", lightingManualMode() ? "true" : "false",
           static_cast<unsigned>(lightingBrightnessPercent(1)),
           static_cast<unsigned>(lightingBrightnessPercent(2)),
           static_cast<unsigned>(lightingBrightnessPercent(3)),
           buzzerEnabled() ? "true" : "false");
  return String(response);
}

esp_err_t actuatorGetHandler(httpd_req_t* request) {
  return sendResponse(request, 200, "application/json", actuatorJson());
}

esp_err_t actuatorPostHandler(httpd_req_t* request) {
  String body;
  if (!readBody(request, body)) {
    return sendResponse(request, 400, "application/json",
                        "{\"ok\":false,\"message\":\"请求正文无效\"}");
  }
  String value;
  if (formValue(body, "mode", value)) {
    if (value == "auto" || value == "servo_auto") servoResumeAutomatic();
    if (value == "auto" || value == "fan_auto") fanResumeAutomatic();
    if (value == "auto" || value == "lighting_auto") lightingResumeAutomatic();
    if (value != "auto" && value != "servo_auto" && value != "fan_auto" &&
        value != "lighting_auto") {
      return sendResponse(request, 400, "application/json",
                          "{\"ok\":false,\"message\":\"未知的自动控制模式\"}");
    }
    return sendResponse(request, 200, "application/json",
                        "{\"ok\":true,\"message\":\"已恢复自动控制\"}");
  }

  bool changed = false;
  String message;
  if (formValue(body, "servo_angle", value) || formValue(body, "angle", value)) {
    const int angle = value.toInt();
    if (angle < 0 || angle > 180)
      return sendResponse(request, 400, "application/json", "{\"ok\":false,\"message\":\"舵机角度必须在 0 到 180 之间\"}");
    if (!servoSetManualAngle(static_cast<uint16_t>(angle)))
      return sendResponse(request, 503, "application/json", "{\"ok\":false,\"message\":\"舵机 PWM 尚未就绪\"}");
    changed = true;
    message = "舵机已转到 " + String(angle) + " 度";
  }

  String directionText;
  String speedText;
  const bool hasDirection = formValue(body, "fan_direction", directionText);
  const bool hasSpeed = formValue(body, "fan_speed", speedText);
  if (hasDirection || hasSpeed) {
    const int direction = hasDirection ? directionText.toInt() : fanDirection();
    const int speed = hasSpeed ? speedText.toInt() : fanSpeedPercent();
    const bool stopCommand = direction == 0 && speed == 0;
    if ((!stopCommand && direction != -1 && direction != 1) || speed < 0 || speed > 100)
      return sendResponse(request, 400, "application/json", "{\"ok\":false,\"message\":\"风扇方向必须为 -1/1，速度为 0 到 100\"}");
    const int8_t appliedDirection = stopCommand ? (fanDirection() == 0 ? 1 : fanDirection()) : direction;
    if (!fanSetManual(appliedDirection, static_cast<uint8_t>(speed)))
      return sendResponse(request, 503, "application/json", "{\"ok\":false,\"message\":\"风扇 PWM 尚未就绪\"}");
    changed = true;
    message += (message.isEmpty() ? "" : "；") + String(speed == 0 ? "风扇已停止" : "风扇已更新");
  }

  String led1Text, led2Text, led3Text;
  const bool hasLed1 = formValue(body, "led1_brightness", led1Text);
  const bool hasLed2 = formValue(body, "led2_brightness", led2Text);
  const bool hasLed3 = formValue(body, "led3_brightness", led3Text);
  if (hasLed1 || hasLed2 || hasLed3) {
    const int led1 = hasLed1 ? led1Text.toInt() : lightingBrightnessPercent(1);
    const int led2 = hasLed2 ? led2Text.toInt() : lightingBrightnessPercent(2);
    const int led3 = hasLed3 ? led3Text.toInt() : lightingBrightnessPercent(3);
    if (led1 < 0 || led1 > 100 || led2 < 0 || led2 > 100 || led3 < 0 || led3 > 100)
      return sendResponse(request, 400, "application/json", "{\"ok\":false,\"message\":\"LED 亮度必须在 0 到 100\"}");
    if (!lightingSetManual(led1, led2, led3))
      return sendResponse(request, 503, "application/json", "{\"ok\":false,\"message\":\"LED PWM 尚未就绪\"}");
    changed = true;
    message += (message.isEmpty() ? "" : "；") + String("LED 亮度已更新");
  }

  if (formValue(body, "buzzer_enabled", value)) {
    if (value != "0" && value != "1" && value != "false" && value != "true")
      return sendResponse(request, 400, "application/json", "{\"ok\":false,\"message\":\"蜂鸣器状态必须为 0 或 1\"}");
    const bool enabled = value == "1" || value == "true";
    if (!buzzerSetEnabled(enabled))
      return sendResponse(request, 503, "application/json", "{\"ok\":false,\"message\":\"蜂鸣器设置保存失败\"}");
    changed = true;
    message += (message.isEmpty() ? "" : "；") + String(enabled ? "蜂鸣器已启用" : "蜂鸣器已停用");
  }

  if (!changed)
    return sendResponse(request, 400, "application/json", "{\"ok\":false,\"message\":\"没有可执行的参数\"}");
  return sendResponse(request, 200, "application/json",
                      String("{\"ok\":true,\"message\":\"") + message + "\"}");
}
}  // namespace

bool LocalHttpsApi::begin() {
  return loadOrCreateCa();
}

bool LocalHttpsApi::loadOrCreateCa() {
  Preferences preferences;
  if (preferences.begin(kTlsNamespace, true)) {
    const size_t keyLength = preferences.getBytesLength(kCaKeyKey);
    const size_t certLength = preferences.getBytesLength(kCaCertKey);
    if (keyLength > 0 && certLength > 0) {
      char* key = new char[keyLength + 1];
      char* cert = new char[certLength + 1];
      const bool read = preferences.getBytes(kCaKeyKey, key, keyLength) == keyLength &&
                        preferences.getBytes(kCaCertKey, cert, certLength) == certLength;
      key[keyLength] = 0;
      cert[certLength] = 0;
      if (read) {
        caPrivateKeyPem_ = key;
        caCertificatePem_ = cert;
      }
      delete[] key;
      delete[] cert;
    }
    preferences.end();
  }
  if (!caPrivateKeyPem_.isEmpty() && !caCertificatePem_.isEmpty()) return true;
  return createCa();
}

bool LocalHttpsApi::createCa() {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context random;
  mbedtls_pk_context key;
  mbedtls_x509write_cert certificate;
  mbedtls_x509write_crt_init(&certificate);
  const bool initialized = seedRandom(entropy, random) && initEcKey(key, random);
  bool ok = initialized && setCommonCertificateFields(certificate, key, key, kCaName, kCaName, 1) &&
            mbedtls_x509write_crt_set_basic_constraints(&certificate, 1, 0) == 0 &&
            mbedtls_x509write_crt_set_key_usage(&certificate,
                MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN |
                MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0 &&
            writePrivateKey(key, caPrivateKeyPem_) &&
            writeCertificate(certificate, random, caCertificatePem_);
  if (ok) {
    Preferences preferences;
    ok = preferences.begin(kTlsNamespace, false) &&
         preferences.putBytes(kCaKeyKey, caPrivateKeyPem_.c_str(), caPrivateKeyPem_.length()) == caPrivateKeyPem_.length() &&
         preferences.putBytes(kCaCertKey, caCertificatePem_.c_str(), caCertificatePem_.length()) == caCertificatePem_.length();
    preferences.end();
  }
  mbedtls_x509write_crt_free(&certificate);
  if (initialized) mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&random);
  mbedtls_entropy_free(&entropy);
  Serial.println(ok ? "[TLS] created persistent local CA" : "[TLS] ERROR: could not create local CA");
  return ok;
}

bool LocalHttpsApi::createServerIdentity(const IPAddress& stationIp) {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context random;
  mbedtls_pk_context caKey;
  mbedtls_pk_context serverKey;
  mbedtls_x509write_cert certificate;
  mbedtls_pk_init(&caKey);
  mbedtls_x509write_crt_init(&certificate);
  const bool randomReady = seedRandom(entropy, random);
  const bool caReady = randomReady && mbedtls_pk_parse_key(
      &caKey, reinterpret_cast<const unsigned char*>(caPrivateKeyPem_.c_str()),
      caPrivateKeyPem_.length() + 1, nullptr, 0) == 0;
  const bool serverReady = caReady && initEcKey(serverKey, random);
  const uint32_t serial = (static_cast<uint32_t>(stationIp[0]) << 24) |
                          (static_cast<uint32_t>(stationIp[1]) << 16) |
                          (static_cast<uint32_t>(stationIp[2]) << 8) | stationIp[3];
  bool ok = serverReady &&
      setCommonCertificateFields(certificate, serverKey, caKey, kServerName, kCaName, serial) &&
      mbedtls_x509write_crt_set_basic_constraints(&certificate, 0, -1) == 0 &&
      mbedtls_x509write_crt_set_key_usage(&certificate,
          MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_AGREEMENT) == 0 &&
      addServerSan(certificate, stationIp) && addServerExtendedKeyUsage(certificate) &&
      writePrivateKey(serverKey, serverPrivateKeyPem_) &&
      writeCertificate(certificate, random, serverCertificatePem_);
  mbedtls_x509write_crt_free(&certificate);
  if (serverReady) mbedtls_pk_free(&serverKey);
  mbedtls_pk_free(&caKey);
  mbedtls_ctr_drbg_free(&random);
  mbedtls_entropy_free(&entropy);
  return ok;
}

bool LocalHttpsApi::start(const IPAddress& stationIp) {
  stop();
  lastStartAttemptMs_ = millis();
  if (caCertificatePem_.isEmpty() && !loadOrCreateCa()) return false;
  if (!createServerIdentity(stationIp)) {
    Serial.println("[TLS] ERROR: certificate generation failed");
    return false;
  }
  httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
  config.httpd.max_uri_handlers = 8;
  config.httpd.max_open_sockets = 2;
  config.httpd.stack_size = 12288;
  config.cacert_pem = reinterpret_cast<const uint8_t*>(serverCertificatePem_.c_str());
  config.cacert_len = serverCertificatePem_.length() + 1;
  config.prvtkey_pem = reinterpret_cast<const uint8_t*>(serverPrivateKeyPem_.c_str());
  config.prvtkey_len = serverPrivateKeyPem_.length() + 1;
  if (httpd_ssl_start(&server_, &config) != ESP_OK) {
    server_ = nullptr;
    Serial.println("[TLS] ERROR: HTTPS server start failed");
    return false;
  }
  httpd_uri_t routes[] = {
      {.uri = "/api/status", .method = HTTP_GET, .handler = statusHandler, .user_ctx = this},
      {.uri = "/api/status", .method = HTTP_OPTIONS, .handler = optionsHandler, .user_ctx = this},
      {.uri = "/api/actuator", .method = HTTP_GET, .handler = actuatorGetHandler, .user_ctx = this},
      {.uri = "/api/actuator", .method = HTTP_POST, .handler = actuatorPostHandler, .user_ctx = this},
      {.uri = "/api/actuator", .method = HTTP_OPTIONS, .handler = optionsHandler, .user_ctx = this},
  };
  for (const httpd_uri_t& route : routes) {
    if (httpd_register_uri_handler(server_, &route) != ESP_OK) {
      stop();
      Serial.println("[TLS] ERROR: HTTPS route registration failed");
      return false;
    }
  }
  certificateIp_ = stationIp;
  Serial.printf("[TLS] HTTPS API ready: https://%s (certificate SAN includes current IP)\r\n",
                stationIp.toString().c_str());
  return true;
}

void LocalHttpsApi::update(const SensorData& latestData, bool stationConnected,
                           const IPAddress& stationIp) {
  latestData_ = latestData;
  if (!stationConnected) {
    stop();
    return;
  }
  if (server_ != nullptr && certificateIp_ == stationIp) return;
  if (server_ != nullptr || lastStartAttemptMs_ == 0 ||
      millis() - lastStartAttemptMs_ >= kStartRetryMs) {
    start(stationIp);
  }
}

void LocalHttpsApi::stop() {
  if (server_ != nullptr) {
    httpd_ssl_stop(server_);
    server_ = nullptr;
  }
}

String LocalHttpsApi::httpsBaseUrl() const {
  return ready() ? String("https://") + certificateIp_.toString() : String();
}
