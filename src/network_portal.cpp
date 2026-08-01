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
// 首次配网在没有任何手机连接热点时，1 分钟后自动进入离线监测模式。
constexpr uint32_t kProvisioningNoClientTimeoutMs = 60000;

const char kWebPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>教室环境监测器</title><style>
body{max-width:680px;margin:24px auto;padding:0 16px;font:16px system-ui;color:#172033;background:#f5f7fb}h1{font-size:1.6rem}.card{background:#fff;border-radius:12px;padding:16px;margin:12px 0;box-shadow:0 2px 10px #0001}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.value{font-size:1.25rem;font-weight:700}label,input,select,button{display:block;width:100%;box-sizing:border-box;margin:8px 0}input,select,button{padding:10px;border-radius:7px;border:1px solid #ccd3df}button{background:#2563eb;color:white;border:0;font-weight:700}.muted{color:#596579;font-size:.9rem}</style>
<body><h1>教室环境监测器</h1><p id="network" class="muted">加载中…</p>
<section class="card"><h2>实时数据</h2><div class="grid"><div>光照<div id="light" class="value">--</div></div><div>空气<div id="air" class="value">--</div></div><div>温度<div id="temperature" class="value">--</div></div><div>湿度<div id="humidity" class="value">--</div></div></div></section>
<section class="card"><h2>Wi-Fi 配网</h2><p class="muted">扫描后从下拉框选择附近网络；隐藏网络可选“手动输入”。保存后设备将关闭配网热点并尝试连接。</p><button id="scanButton" type="button" onclick="networks()">扫描附近 Wi-Fi</button><p id="scanStatus" class="muted">尚未扫描</p><form method="post" action="/api/wifi"><label>Wi-Fi 名称（SSID）<select id="ssidSelect" name="ssid" onchange="chooseSsid()"><option value="">请先扫描并选择</option><option value="__manual__">手动输入隐藏网络…</option></select><input id="ssidManual" maxlength="32" placeholder="输入隐藏网络名称" hidden></label><label>密码（开放网络可留空）<input name="password" type="password" maxlength="63"></label><button type="submit">保存并连接</button></form><form method="post" action="/api/wifi/reset" onsubmit="return confirm('清除已保存的 Wi-Fi？')"><button type="submit">清除 Wi-Fi，重新配网</button></form></section>
<script>const unit={light:'%',air:' ppm',temperature:' °C',humidity:'%'};function val(v,k){return v===null?'--':v+unit[k]}async function refresh(){try{let s=await (await fetch('/api/status',{cache:'no-store'})).json();for(let k of Object.keys(unit))document.getElementById(k).textContent=val(s[k],k);document.getElementById('network').textContent=s.provisioning?'配网热点：'+s.apSsid+'（无密码）':'设备 IP：'+s.ip}catch(e){document.getElementById('network').textContent='读取设备状态失败'}}function chooseSsid(){let s=document.getElementById('ssidSelect'),m=document.getElementById('ssidManual'),manual=s.value==='__manual__';m.hidden=!manual;m.name=manual?'ssid':'';s.name=manual?'':'ssid';m.required=manual}async function networks(){let b=document.getElementById('scanButton'),t=document.getElementById('scanStatus'),s=document.getElementById('ssidSelect');try{b.disabled=true;b.textContent='正在扫描…';t.textContent='正在扫描附近 Wi-Fi，请稍候…';let n=await (await fetch('/api/networks')).json();if(n.scanning){setTimeout(networks,800);return}s.replaceChildren(new Option('请选择 Wi-Fi',''),new Option('手动输入隐藏网络…','__manual__'));n.networks.forEach(name=>s.add(new Option(name,name)));t.textContent=n.networks.length?'扫描完成，找到 '+n.networks.length+' 个网络。':'扫描完成，未找到网络。';b.disabled=false;b.textContent='重新扫描'}catch(e){t.textContent='扫描失败，请重试。';b.disabled=false;b.textContent='扫描附近 Wi-Fi'}}refresh();setInterval(refresh,2000);</script></body></html>)HTML";

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
  connecting_ = false;
  connectionSucceeded_ = false;
  provisioningStartedMs_ = millis();
  provisioningClientSeen_ = false;
  networkListJson_ = "{\"networks\":[],\"scanning\":false}";
  // 固定热点名较长；二维码编码入口 URL 可保持足够大的模块和白边，扫描更可靠。
  drawProvisioningScreen(apSsid_.c_str(), "http://192.168.4.1/");
  Serial.printf("[WiFi] provisioning AP: %s (open), portal: http://%s\n",
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
  connecting_ = true;
  connectionSucceeded_ = false;
  drawNetworkConnectingScreen(ssid.c_str());
  Serial.printf("[WiFi] connecting to saved SSID: %s\n", ssid.c_str());
}

void NetworkPortal::startOffline() {
  dnsServer_.stop();
  WiFi.disconnect(false, false);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  provisioning_ = false;
  connecting_ = false;
  Serial.println("[WiFi] offline mode selected; network is only retried on next boot");
}

void NetworkPortal::begin() {
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
  String response = "{\"light\":" + jsonNumber(data.lightPercent, 0) +
                    ",\"air\":" + (data.mq135WarmedUp ? jsonNumber(data.mq135PpmEstimate, 0) : "null") +
                    ",\"temperature\":" + (data.dhtValid ? jsonNumber(data.temperatureC, 1) : "null") +
                    ",\"humidity\":" + (data.dhtValid ? jsonNumber(data.humidityPercent, 0) : "null") +
                    ",\"provisioning\":" + (provisioning_ ? "true" : "false") +
                    ",\"apSsid\":\"" + apSsid_ + "\",\"ip\":\"" +
                    (provisioning_ ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\"}";
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", response);
}

void NetworkPortal::sendNetworks() {
  if (!provisioning_) {
    server_.send(409, "application/json", "{\"networks\":[],\"scanning\":false}");
    return;
  }
  // 仅由 WebUI 的“扫描附近 Wi-Fi”按钮触发；不再在后台周期性扫描。
  if (!networkScanRunning_) {
    networkListJson_ = "{\"networks\":[],\"scanning\":true}";
    startNetworkScan();
  }
  server_.send(200, "application/json", networkListJson_);
}

void NetworkPortal::startNetworkScan() {
  if (!provisioning_ || networkScanRunning_) return;
  // 必须异步扫描：同步扫全信道会堵住 OLED、传感器和串口数秒。
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true);
  networkScanRunning_ = true;
  lastNetworkScanMs_ = millis();
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
    // 配网页始终占屏，不再循环切换。只要有设备曾连上热点，就交由用户完成
    // 配网或手动选择“暂不连接”；无人连接才在一分钟后自动退出。
    if (WiFi.softAPgetStationNum() > 0) provisioningClientSeen_ = true;
    if (!provisioningClientSeen_ && millis() - provisioningStartedMs_ >= kProvisioningNoClientTimeoutMs) {
      Serial.println("[WiFi] no provisioning client after 60s; entering offline mode");
      startOffline();
      return;
    }

    if (networkScanRunning_) {
      const int found = WiFi.scanComplete();
      if (found >= 0) {
        String response = "{\"networks\":[";
        for (int i = 0; i < found; ++i) {
          if (i != 0) response += ',';
          response += '"';
          response += escapeJson(WiFi.SSID(i));
          response += '"';
        }
        networkListJson_ = response + "],\"scanning\":false}";
        WiFi.scanDelete();
        networkScanRunning_ = false;
        lastNetworkScanMs_ = millis();
      } else if (found == WIFI_SCAN_FAILED) {
        networkListJson_ = "{\"networks\":[],\"scanning\":false}";
        networkScanRunning_ = false;
        lastNetworkScanMs_ = millis();
      }
    }
  }
  server_.handleClient();

  // 先让 POST 请求完成响应，再切换无线模式，否则手机会在收到确认前断连。
  if (pendingStationStart_) {
    pendingStationStart_ = false;
    startStation(pendingSsid_, pendingPassword_);
  }
  if (!connecting_) return;

  if (WiFi.status() == WL_CONNECTED) {
    connecting_ = false;
    connectionSucceeded_ = true;
    Serial.printf("[WiFi] connected: http://%s\n", WiFi.localIP().toString().c_str());
  } else if (millis() - connectionStartedMs_ >= kConnectionTimeoutMs) {
    Serial.println("[WiFi] connection timed out; reopening provisioning AP");
    startProvisioning();
  }
}

bool NetworkPortal::isShowingNetworkScreen() const {
  return provisioning_ || connecting_;
}

bool NetworkPortal::takeConnectionSucceeded() {
  const bool succeeded = connectionSucceeded_;
  connectionSucceeded_ = false;
  return succeeded;
}

void NetworkPortal::handleProvisioningInput(bool shortPress) {
  // OLED 的光标固定停在“暂不连接”，短按即确认；该操作不通过 WebUI 暴露。
  if (provisioning_ && shortPress) startOffline();
}
