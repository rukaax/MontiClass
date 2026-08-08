/** @file network_portal.h
 * @brief Wi-Fi 本地配网热点、WebUI 与凭据存储。
 */
#pragma once

#include <DNSServer.h>
#include <WebServer.h>

#include "report_uploader.h"
#include "rtc_clock.h"
#include "sensors.h"
#include "thresholds.h"

class NetworkPortal {
 public:
  // 自动读取本地凭据：没有凭据时创建无密码配网热点。
  void begin();
  void setThresholds(Thresholds& thresholds);
  // 每次 loop 调用，维护 HTTP/DNS 请求及 Wi-Fi 连接状态。
  void update(const SensorData& latestData);
  // 成功连上路由器后只返回一次 true，供主界面切回监测页。
  bool takeConnectionSucceeded();
  // 配网或连接中时，主循环不应覆盖 OLED 上的网络提示页。
  bool isShowingNetworkScreen() const;
  // 设置页只读网络信息；未连接时地址为空。
  bool isStationConnected() const;
  String stationIpAddress() const;
  ReportUploadStatus reportUploadStatus() const;

 private:
  void configureRoutes();
  void startProvisioning();
  void startStation(const String& ssid, const String& password);
  bool loadCredentials(String& ssid, String& password);
  bool saveCredentials(const String& ssid, const String& password);
  void clearCredentials();
  void sendStatus();
  void sendLightingConfig();
  void receiveLightingConfig();
  void sendTime();
  void receiveTime();
  void sendNetworks();
  bool startNetworkScan();
  void ensureProvisioningAp();
  void receiveCredentials();

  WebServer server_{80};
  DNSServer dnsServer_;
  ReportUploader reportUploader_;
  const SensorData* latestData_ = nullptr;
  Thresholds* thresholds_ = nullptr;
  String apSsid_;
  String pendingSsid_;
  String pendingPassword_;
  uint32_t connectionStartedMs_ = 0;
  bool provisioning_ = false;
  // 一分钟后仅让 OLED 回到监测页；AP、DNS 和 WebUI 仍保持运行。
  bool provisioningScreenVisible_ = false;
  bool connecting_ = false;
  bool pendingStationStart_ = false;
  bool connectionSucceeded_ = false;
  uint32_t provisioningStartedMs_ = 0;
  uint32_t lastApHealthCheckMs_ = 0;
  bool networkScanRunning_ = false;
  uint8_t networkScanAttempt_ = 0;
  uint32_t lastNetworkScanMs_ = 0;
  String networkListJson_ = "{\"networks\":[],\"scanning\":false,\"error\":\"\"}";
};
