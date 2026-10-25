/** @file local_https.h
 * @brief LAN HTTPS API with persistent CA and IP-bound server identities.
 */
#pragma once

#include <Arduino.h>
#include <esp_http_server.h>

#include "sensors.h"

class LocalHttpsApi {
 public:
  // Creates/loads the device CA. The CA remains stable across reboots.
  bool begin();
  // Starts/stops port 443 and only reissues the certificate when the IP changes.
  void update(const SensorData& latestData, bool stationConnected, const IPAddress& stationIp);
  void stop();

  bool ready() const { return server_ != nullptr; }
  const SensorData& latestData() const { return latestData_; }
  const String& caCertificatePem() const { return caCertificatePem_; }
  String httpsBaseUrl() const;

 private:
  bool loadOrCreateCa();
  bool createCa();
  bool loadServerIdentity(const IPAddress& stationIp);
  bool saveServerIdentity(const IPAddress& stationIp);
  bool createServerIdentity(const IPAddress& stationIp);
  bool loadOrCreateServerIdentity(const IPAddress& stationIp);
  bool start(const IPAddress& stationIp);

  httpd_handle_t server_ = nullptr;
  SensorData latestData_{};
  IPAddress certificateIp_;
  uint32_t lastStartAttemptMs_ = 0;
  String caPrivateKeyPem_;
  String caCertificatePem_;
  String serverPrivateKeyPem_;
  String serverCertificatePem_;
};
