/** @file network_portal.cpp
 * @brief 无密码热点配网、WebUI 和 Wi-Fi 凭据的本地 NVS 存储。
 */
#include "network_portal.h"

#include <Preferences.h>
#include <WiFi.h>

#include <math.h>

#include "SSD1306.h"

namespace {
constexpr char kWifiNamespace[] = "wifi_config";
constexpr char kSsidKey[] = "ssid";
constexpr char kPasswordKey[] = "password";
constexpr uint32_t kConnectionTimeoutMs = 15000;
// 配网提示页显示 1 分钟，之后 OLED 回到监测页，但热点和 WebUI 保留。
constexpr uint32_t kProvisioningScreenTimeoutMs = 60000;
constexpr uint32_t kApHealthCheckIntervalMs = 5000;
constexpr uint8_t kNetworkScanMaxAttempts = 3;

const char kWebPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>教室环境监测器</title><style>
body{max-width:680px;margin:24px auto;padding:0 16px;font:16px system-ui;color:#172033;background:#f5f7fb}h1{font-size:1.6rem}.card{background:#fff;border-radius:12px;padding:16px;margin:12px 0;box-shadow:0 2px 10px #0001}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.value{font-size:1.25rem;font-weight:700}label,input,select,textarea,button{display:block;width:100%;box-sizing:border-box;margin:8px 0}input,select,textarea,button{padding:10px;border-radius:7px;border:1px solid #ccd3df}button{background:#2563eb;color:white;border:0;font-weight:700}.muted{color:#596579;font-size:.9rem}</style>
<body><h1>教室环境监测器</h1><p id="network" class="muted">加载中…</p>
<section class="card"><h2>实时数据</h2><div class="grid"><div>光照 1<div id="light" class="value">--</div></div><div>光照 2<div id="light2" class="value">--</div></div><div>空气 AQI（估算）<div id="air" class="value">--</div><div id="airDetail" class="muted">预热并建立基线中…</div></div><div>温度<div id="temperature" class="value">--</div></div><div>湿度<div id="humidity" class="value">--</div></div><div>人体存在<div id="presence" class="value">--</div></div></div></section>
<section class="card"><h2>Wi-Fi 配网</h2><p class="muted">扫描后从下拉框选择附近网络；隐藏网络可选“手动输入”。保存后设备将关闭配网热点并尝试连接。</p><button id="scanButton" type="button" onclick="networks(true)">扫描附近 Wi-Fi</button><p id="scanStatus" class="muted">尚未扫描</p><form method="post" action="/api/wifi"><label>Wi-Fi 名称（SSID）<select id="ssidSelect" name="ssid" onchange="chooseSsid()"><option value="">请先扫描并选择</option><option value="__manual__">手动输入隐藏网络…</option></select><input id="ssidManual" maxlength="32" placeholder="输入隐藏网络名称" hidden></label><label>密码（开放网络可留空）<input name="password" type="password" maxlength="63"></label><button type="submit">保存并连接</button></form><form method="post" action="/api/wifi/reset" onsubmit="return confirm('清除已保存的 Wi-Fi？')"><button type="submit">清除 Wi-Fi，重新配网</button></form></section>
<section class="card"><h2>数据上报</h2><p class="muted">设备 ID：<span id="reportDeviceId">--</span></p><form id="reportingForm" onsubmit="saveReporting(event)"><label>API Base URL<input id="reportBaseUrl" name="base_url" type="url" maxlength="240" placeholder="http://192.168.1.100:3001/api" required></label><label>设备 Token<input id="reportToken" name="token" type="password" maxlength="512" placeholder="已配置时留空可保留"></label><label>数据上报间隔（秒）<input id="reportInterval" name="report_interval" type="number" min="1" max="86400" step="1" required></label><label>心跳间隔（秒，0=关闭）<input id="heartbeatInterval" name="heartbeat_interval" type="number" min="0" max="86400" step="1" required></label><label>自定义请求头（每行：名称: 值）<textarea id="reportHeaders" name="headers" maxlength="1024" rows="4" placeholder="X-Classroom: 101"></textarea></label><p class="muted">Authorization、Content-Type、Host 等鉴权和协议头由设备自动管理。</p><button type="submit">保存上报配置</button></form><div class="grid"><button type="button" onclick="manualReport()">立即上报当前数据</button><button type="button" onclick="clearReportLogs()">清空回调日志</button></div><p id="reportStatus" class="muted">读取配置中…</p><pre id="reportLogs" class="muted" style="white-space:pre-wrap;max-height:260px;overflow:auto">暂无回调日志</pre></section>
<script>const unit={light:'%',light2:'%',air:' AQI',temperature:' °C',humidity:'%'};function val(v,k){return v===null?'--':v+unit[k]}async function refresh(){try{let s=await (await fetch('/api/status',{cache:'no-store'})).json();for(let k of Object.keys(unit))document.getElementById(k).textContent=val(s[k],k);document.getElementById('presence').textContent=s.presence?'有人':'无人';document.getElementById('airDetail').textContent=s.airReady?'基线 ADC '+s.airBaseline+' · 当前 '+s.airRaw+' / GPIO '+s.airMv+' mV':'预热并建立基线中 · ADC '+s.airRaw+' / GPIO '+s.airMv+' mV';document.getElementById('network').textContent=s.provisioning?'配网热点：'+s.apSsid+'（无密码）':'设备 IP：'+s.ip}catch(e){document.getElementById('network').textContent='读取设备状态失败'}}function chooseSsid(){let s=document.getElementById('ssidSelect'),m=document.getElementById('ssidManual'),manual=s.value==='__manual__';m.hidden=!manual;m.name=manual?'ssid':'';s.name=manual?'':'ssid';m.required=manual}async function networks(start=false){let b=document.getElementById('scanButton'),t=document.getElementById('scanStatus'),s=document.getElementById('ssidSelect');try{b.disabled=true;b.textContent='正在扫描…';t.textContent='正在扫描附近 Wi-Fi，请稍候…';let n=await (await fetch('/api/networks'+(start?'?start=1':''),{cache:'no-store'})).json();if(n.scanning){setTimeout(()=>networks(false),800);return}if(n.error)throw new Error(n.error);s.replaceChildren(new Option('请选择 Wi-Fi',''),new Option('手动输入隐藏网络…','__manual__'));n.networks.forEach(name=>s.add(new Option(name,name)));t.textContent=n.networks.length?'扫描完成，找到 '+n.networks.length+' 个网络。':'扫描完成，未找到网络。';b.disabled=false;b.textContent='重新扫描'}catch(e){t.textContent='扫描失败，请重试。';b.disabled=false;b.textContent='扫描附近 Wi-Fi'}}async function reporting(){let t=document.getElementById('reportStatus');try{let r=await (await fetch('/api/reporting',{cache:'no-store'})).json();document.getElementById('reportDeviceId').textContent=r.deviceId;document.getElementById('reportBaseUrl').value=r.baseUrl;if(!r.configured)t.textContent='尚未配置';else if(r.lastHttpCode===201)t.textContent='已配置，上次上报成功';else if(r.lastHttpCode>0)t.textContent='已配置，上次上报 HTTP '+r.lastHttpCode;else if(r.lastHttpCode<0)t.textContent='已配置，上次上报网络错误 '+r.lastHttpCode;else t.textContent='已配置，等待上报'}catch(e){t.textContent='读取上报配置失败'}}async function saveReporting(e){e.preventDefault();let t=document.getElementById('reportStatus');try{t.textContent='正在保存…';let r=await fetch('/api/reporting',{method:'POST',body:new URLSearchParams(new FormData(e.target))});let j=await r.json();if(!r.ok||!j.ok)throw new Error(j.error);document.getElementById('reportToken').value='';t.textContent='保存成功，联网后将自动上报';setTimeout(reporting,1200)}catch(e){t.textContent='保存失败，请检查地址和 Token'}}refresh();reporting();setInterval(refresh,2000);setInterval(reporting,10000);</script><script>async function loadReportControls(){try{let r=await (await fetch('/api/reporting',{cache:'no-store'})).json();document.getElementById('reportInterval').value=r.reportIntervalSec;document.getElementById('heartbeatInterval').value=r.heartbeatIntervalSec;document.getElementById('reportHeaders').value=r.customHeaders||''}catch(e){}}const originalSaveReporting=saveReporting;saveReporting=async function(e){await originalSaveReporting(e);setTimeout(loadReportControls,1200)}async function manualReport(){let t=document.getElementById('reportStatus');try{t.textContent='正在上报当前数据…';let r=await fetch('/api/reporting/send',{method:'POST'}),j=await r.json();t.textContent=r.ok&&j.ok?'手动上报已排队':('手动上报失败：'+(j.error||r.status))}catch(e){t.textContent='手动上报请求失败'}}async function reportingLogs(){try{let r=await (await fetch('/api/reporting/logs',{cache:'no-store'})).json(),p=document.getElementById('reportLogs');if(!r.length){p.textContent='暂无回调日志';return}p.textContent=r.slice().reverse().map(x=>'启动后 '+Math.floor(x.timeMs/1000)+'s ['+x.kind+'] HTTP '+x.code+' bytes='+x.requestBytes+'\n'+x.response).join('\n\n')}catch(e){}}async function clearReportLogs(){await fetch('/api/reporting/logs/clear',{method:'POST'});reportingLogs()}loadReportControls();reportingLogs();setInterval(loadReportControls,10000);setInterval(reportingLogs,3000);</script></body></html>)HTML";

String jsonNumber(float value, uint8_t digits) {
  return isnan(value) ? "null" : String(value, static_cast<unsigned int>(digits));
}

String escapeJson(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 4);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '"' || c == '\\') escaped += '\\';
    if (c == '\n') escaped += "\\n";
    else if (c == '\r') escaped += "\\r";
    else if (c != '\n') escaped += c;
  }
  return escaped;
}
}  // namespace

void NetworkPortal::configureRoutes() {
  reportUploader_.configureRoutes(server_);
  server_.on("/", HTTP_GET, [this]() { server_.send_P(200, "text/html; charset=utf-8", kWebPage); });
  server_.on("/api/status", HTTP_GET, [this]() { sendStatus(); });
  server_.on("/api/networks", HTTP_GET, [this]() { sendNetworks(); });
  server_.on("/api/wifi", HTTP_POST, [this]() { receiveCredentials(); });
  server_.on("/api/wifi/reset", HTTP_POST, [this]() {
    clearCredentials();
    server_.send(200, "text/html; charset=utf-8", "<p>已清除 Wi-Fi。热点正在重新开启，请稍候。</p>");
    pendingSsid_ = "";
    pendingPassword_ = "";
    pendingStationStart_ = false;
    startProvisioning();
  });
  // 常见系统探测地址也返回入口页，让连接热点后的“登录网络”提示可直接进入 WebUI。
  server_.on("/generate_204", HTTP_GET, [this]() { server_.sendHeader("Location", "/"); server_.send(302); });
  server_.on("/hotspot-detect.html", HTTP_GET, [this]() { server_.sendHeader("Location", "/"); server_.send(302); });
  server_.onNotFound([this]() { server_.sendHeader("Location", "/"); server_.send(302); });
}

bool NetworkPortal::loadCredentials(String& ssid, String& password) {
  Preferences preferences;
  if (!preferences.begin(kWifiNamespace, true)) return false;
  ssid = preferences.getString(kSsidKey, "");
  password = preferences.getString(kPasswordKey, "");
  preferences.end();
  return !ssid.isEmpty();
}

bool NetworkPortal::saveCredentials(const String& ssid, const String& password) {
  Preferences preferences;
  if (!preferences.begin(kWifiNamespace, false)) return false;
  const bool ssidSaved = preferences.putString(kSsidKey, ssid) == ssid.length();
  const bool passwordSaved = preferences.putString(kPasswordKey, password) == password.length();
  preferences.end();
  return ssidSaved && passwordSaved;
}

void NetworkPortal::clearCredentials() {
  Preferences preferences;
  if (preferences.begin(kWifiNamespace, false)) {
    preferences.clear();
    preferences.end();
  }
}

void NetworkPortal::startProvisioning() {
  // AP+STA 允许配网页在不掉热点的情况下扫描附近路由器。
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  // 只有 Wi-Fi 模式启动后，lwIP 的邮箱才已建立；此时才能安全断开旧 STA。
  WiFi.disconnect(false, false);
  WiFi.scanDelete();
  networkScanRunning_ = false;
  apSsid_ = "RX-Classroom-demo";

  // 不传第二个参数即创建开放热点，符合首次配网“不设密码”的要求。
  if (!WiFi.softAP(apSsid_.c_str())) {
    Serial.println("[WiFi] ERROR: could not start provisioning AP");
    return;
  }
  dnsServer_.start(53, "*", WiFi.softAPIP());
  provisioning_ = true;
  provisioningScreenVisible_ = true;
  connecting_ = false;
  connectionSucceeded_ = false;
  provisioningStartedMs_ = millis();
  lastApHealthCheckMs_ = provisioningStartedMs_;
  networkListJson_ = "{\"networks\":[],\"scanning\":false,\"error\":\"\"}";
  // 固定热点名较长；二维码编码入口 URL 可保持足够大的模块和白边，扫描更可靠。
  drawProvisioningScreen(apSsid_.c_str(), "http://192.168.4.1/");
  Serial.printf("[WiFi] provisioning AP: %s (open), portal: http://%s\r\n",
                apSsid_.c_str(), WiFi.softAPIP().toString().c_str());
}

void NetworkPortal::startStation(const String& ssid, const String& password) {
  // 上电首次进入这里时无线栈还未创建，必须先设置模式再调用 AP 操作。
  WiFi.mode(WIFI_STA);
  dnsServer_.stop();
  WiFi.softAPdisconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  pendingSsid_ = ssid;
  pendingPassword_ = password;
  connectionStartedMs_ = millis();
  provisioning_ = false;
  provisioningScreenVisible_ = false;
  connecting_ = true;
  connectionSucceeded_ = false;
  drawNetworkConnectingScreen(ssid.c_str());
  Serial.printf("[WiFi] connecting to saved SSID: %s\r\n", ssid.c_str());
}

void NetworkPortal::begin() {
  reportUploader_.begin();
  configureRoutes();
  // persistent() 不写入 Arduino Wi-Fi 默认存储，凭据只由本模块的 NVS 管理。
  WiFi.persistent(false);
  String ssid;
  String password;
  if (loadCredentials(ssid, password)) startStation(ssid, password);
  else startProvisioning();
  // 不在运行中自动重连；一次联网尝试只发生在启动或用户主动提交配网时。
  WiFi.setAutoReconnect(false);
  // Wi-Fi 模式已在上面建立后才启动 HTTP 服务，避免 lwIP 的 Invalid mbox 断言。
  server_.begin();
}

void NetworkPortal::sendStatus() {
  const SensorData emptyData{};
  const SensorData& data = latestData_ == nullptr ? emptyData : *latestData_;
  String response = "{\"light\":" + jsonNumber(data.light1Percent, 0) +
                    ",\"light2\":" + jsonNumber(data.light2Percent, 0) +
                    ",\"air\":" + (data.mq135WarmedUp ? jsonNumber(data.mq135AqiEstimate, 0) : "null") +
                    ",\"airRaw\":" + String(data.mq135Raw) +
                    ",\"airMv\":" + String(data.mq135MilliVolts) +
                    ",\"airBaseline\":" + String(data.mq135BaselineRaw) +
                    ",\"airReady\":" + (data.mq135WarmedUp ? "true" : "false") +
                    ",\"temperature\":" + (data.dhtValid ? jsonNumber(data.temperatureC, 1) : "null") +
                    ",\"humidity\":" + (data.dhtValid ? jsonNumber(data.humidityPercent, 0) : "null") +
                    ",\"presence\":" + (data.presenceDetected ? "true" : "false") +
                    ",\"provisioning\":" + (provisioning_ ? "true" : "false") +
                    ",\"apSsid\":\"" + apSsid_ + "\",\"ip\":\"" +
                    (provisioning_ ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\"}";
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", response);
}

void NetworkPortal::sendNetworks() {
  if (!provisioning_) {
    server_.send(409, "application/json", "{\"networks\":[],\"scanning\":false,\"error\":\"not_provisioning\"}");
    return;
  }
  // start=1 只由按钮发出；后续轮询不再误触发新扫描或覆盖完成结果。
  if (server_.hasArg("start") && server_.arg("start") == "1" && !networkScanRunning_) {
    networkListJson_ = "{\"networks\":[],\"scanning\":true}";
    networkScanAttempt_ = 0;
    startNetworkScan();
  }
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", networkListJson_);
}

bool NetworkPortal::startNetworkScan() {
  if (!provisioning_ || networkScanRunning_) return false;
  // 必须异步扫描：同步扫全信道会堵住 OLED、传感器和串口数秒。
  WiFi.scanDelete();
  ++networkScanAttempt_;
  // 200ms/信道可覆盖全部 2.4GHz 信道，同时缩短 AP 暂停发 beacon 的时间。
  const int scanState = WiFi.scanNetworks(true, false, false, 200);
  if (scanState != WIFI_SCAN_RUNNING) {
    Serial.printf("[WiFi] scan start failed: attempt=%u/%u state=%d mode=%d\r\n",
                  networkScanAttempt_, kNetworkScanMaxAttempts, scanState,
                  static_cast<int>(WiFi.getMode()));
    if (networkScanAttempt_ < kNetworkScanMaxAttempts) return startNetworkScan();
    networkListJson_ = "{\"networks\":[],\"scanning\":false,\"error\":\"scan_start_failed\"}";
    return false;
  }
  networkScanRunning_ = true;
  lastNetworkScanMs_ = millis();
  Serial.printf("[WiFi] network scan started: attempt=%u/%u stations=%u\r\n",
                networkScanAttempt_, kNetworkScanMaxAttempts, WiFi.softAPgetStationNum());
  return true;
}

void NetworkPortal::ensureProvisioningAp() {
  if (!provisioning_) return;
  const wifi_mode_t mode = WiFi.getMode();
  const bool apModeActive = mode == WIFI_AP || mode == WIFI_AP_STA;
  if (apModeActive && WiFi.softAPSSID() == apSsid_) return;

  Serial.printf("[WiFi] AP watchdog restoring hotspot: mode=%d ssid=%s\r\n",
                static_cast<int>(mode), WiFi.softAPSSID().c_str());
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAP(apSsid_.c_str());
  dnsServer_.stop();
  dnsServer_.start(53, "*", WiFi.softAPIP());
}

void NetworkPortal::receiveCredentials() {
  const String ssid = server_.arg("ssid");
  const String password = server_.arg("password");
  if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63 ||
      (!password.isEmpty() && password.length() < 8)) {
    server_.send(400, "text/html; charset=utf-8", "<p>Wi-Fi 名称或密码格式不正确。</p><a href='/'>返回</a>");
    return;
  }
  if (!saveCredentials(ssid, password)) {
    server_.send(500, "text/html; charset=utf-8", "<p>保存 Wi-Fi 失败，请重试。</p>");
    return;
  }
  pendingSsid_ = ssid;
  pendingPassword_ = password;
  pendingStationStart_ = true;
  server_.send(200, "text/html; charset=utf-8", "<p>已保存，设备正在连接 Wi-Fi。热点会关闭。</p>");
}

void NetworkPortal::update(const SensorData& latestData) {
  latestData_ = &latestData;
  if (provisioning_) {
    dnsServer_.processNextRequest();
    if (millis() - lastApHealthCheckMs_ >= kApHealthCheckIntervalMs) {
      lastApHealthCheckMs_ = millis();
      ensureProvisioningAp();
    }
    // 一分钟后只释放 OLED 给监测页面，不能关闭 AP；用户仍可随时连接热点
    // RX-Classroom-demo 并访问 192.168.4.1 完成配网。
    if (provisioningScreenVisible_ &&
        millis() - provisioningStartedMs_ >= kProvisioningScreenTimeoutMs) {
      provisioningScreenVisible_ = false;
      Serial.println("[WiFi] provisioning screen hidden after 60s; AP and portal remain active");
    }

    if (networkScanRunning_) {
      const int found = WiFi.scanComplete();
      if (found >= 0) {
        String response = "{\"networks\":[";
        int appended = 0;
        for (int i = 0; i < found; ++i) {
          const String ssid = WiFi.SSID(i);
          if (ssid.isEmpty()) continue;
          bool duplicate = false;
          for (int j = 0; j < i; ++j) {
            if (WiFi.SSID(j) == ssid) {
              duplicate = true;
              break;
            }
          }
          if (duplicate) continue;
          if (appended++ != 0) response += ',';
          response += '"';
          response += escapeJson(ssid);
          response += '"';
        }
        networkListJson_ = response + "],\"scanning\":false,\"error\":\"\"}";
        WiFi.scanDelete();
        networkScanRunning_ = false;
        lastNetworkScanMs_ = millis();
        Serial.printf("[WiFi] scan completed: %d AP records, %d unique SSIDs\r\n", found, appended);
      } else if (found == WIFI_SCAN_FAILED) {
        const uint32_t elapsedMs = millis() - lastNetworkScanMs_;
        networkScanRunning_ = false;
        WiFi.scanDelete();
        Serial.printf("[WiFi] scan failed: attempt=%u/%u elapsed=%lums mode=%d stations=%u\r\n",
                      networkScanAttempt_, kNetworkScanMaxAttempts,
                      static_cast<unsigned long>(elapsedMs), static_cast<int>(WiFi.getMode()),
                      WiFi.softAPgetStationNum());
        if (networkScanAttempt_ < kNetworkScanMaxAttempts) {
          networkListJson_ = "{\"networks\":[],\"scanning\":true}";
          startNetworkScan();
        } else {
          networkListJson_ = "{\"networks\":[],\"scanning\":false,\"error\":\"scan_failed\"}";
        }
      }
    }
  }
  server_.handleClient();

  // 先让 POST 请求完成响应，再切换无线模式，否则手机会在收到确认前断连。
  if (pendingStationStart_) {
    pendingStationStart_ = false;
    startStation(pendingSsid_, pendingPassword_);
  }
  if (connecting_) {
    if (WiFi.status() == WL_CONNECTED) {
      connecting_ = false;
      connectionSucceeded_ = true;
      Serial.printf("[WiFi] connected: http://%s\r\n", WiFi.localIP().toString().c_str());
    } else if (millis() - connectionStartedMs_ >= kConnectionTimeoutMs) {
      Serial.println("[WiFi] connection timed out; reopening provisioning AP");
      startProvisioning();
    }
  }
  reportUploader_.update(latestData);
}

bool NetworkPortal::isShowingNetworkScreen() const {
  return (provisioning_ && provisioningScreenVisible_) || connecting_;
}

bool NetworkPortal::isStationConnected() const {
  return !provisioning_ && !connecting_ && WiFi.status() == WL_CONNECTED;
}

String NetworkPortal::stationIpAddress() const {
  return isStationConnected() ? WiFi.localIP().toString() : String();
}

bool NetworkPortal::takeConnectionSucceeded() {
  const bool succeeded = connectionSucceeded_;
  connectionSucceeded_ = false;
  return succeeded;
}
