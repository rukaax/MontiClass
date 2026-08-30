/** @file network_portal.cpp
 * @brief 无密码热点配网、WebUI 和 Wi-Fi 凭据的本地 NVS 存储。
 *
 * 阅读顺序建议：
 *   1. begin() 是入口，决定“进入配网”还是“连接已保存的 Wi-Fi”。
 *   2. startProvisioning() / startStation() 是两条主路径的实现。
 *   3. update() 是主循环调用，负责推动状态机、处理 HTTP/DNS、异步扫描收割。
 *   4. configureRoutes() 及各个 send / receive 方法是 WebUI 的路由与处理器。
 *   前面 kWebPage 之前是内嵌的 HTML/CSS/JS 页面文本，只是字符串常量。
 */
#include "network_portal.h"

#include <Preferences.h>     // ESP32 NVS 读写库，用于持久化 Wi-Fi 凭据
#include <WiFi.h>            // Arduino WiFi 类：AP/STA、扫描、状态

#include <math.h>            // isnan()
#include <stdio.h>           // sscanf / snprintf

#include "SSD1306.h"         // 配网/连接提示屏幕的绘制函数
#include "buzzer.h"          // WebUI 里控制蜂鸣器开关
#include "fan.h"              // WebUI 里查询/覆盖风扇状态
#include "lighting.h"        // WebUI 里查询/覆盖 LED 状态
#include "servo.h"           // WebUI 里查询/覆盖舵机角度
#include "system_log.h"      // 启动诊断日志的 WebUI 查询
#include "threshold_store.h" // 光照阈值和调光时段的持久化

namespace {
// ===== NVS 存储 & 时间常量 =====
constexpr char kWifiNamespace[] = "wifi_config";           // Wi-Fi 凭据的 NVS 命名空间
constexpr char kSsidKey[] = "ssid";                        // 保存 SSID 的键
constexpr char kPasswordKey[] = "password";                // 保存密码的键
constexpr uint32_t kConnectionTimeoutMs = 15000;           // 连接 STA 超时 15s，超时回落到配网
// 配网提示页显示 1 分钟，之后 OLED 回到监测页，但热点和 WebUI 保留。
constexpr uint32_t kProvisioningScreenTimeoutMs = 60000;
constexpr uint32_t kApHealthCheckIntervalMs = 5000;        // 每 5s 检查一次 AP 是否还在
constexpr uint8_t kNetworkScanMaxAttempts = 3;             // 扫描失败时最多重试 3 次
// scanComplete() 在射频被 AP/STA 抢占时可能长期停在 -1，必须自己兜底，
// 否则 networkScanRunning_ 永久为真，之后所有扫描请求都被拒绝。
constexpr uint32_t kNetworkScanTimeoutMs = 12000;

// 把 "HH:MM" 解析成一天中的分钟数（0..1439）；不合法返回 false。
bool parseTimeOfDay(const String& text, uint16_t& minuteOfDay) {
  unsigned int hour = 0;
  unsigned int minute = 0;
  char trailing = '\0';                             // 只允许恰好两个数字段，多余字符判失败
  if (sscanf(text.c_str(), "%u:%u%c", &hour, &minute, &trailing) != 2 ||
      hour > 23 || minute > 59) {
    return false;
  }
  minuteOfDay = static_cast<uint16_t>(hour * 60U + minute);
  return true;
}

// 反向：把一天中的分钟数格式化成 "HH:MM"，写入调用方提供的缓冲。
void formatTimeOfDay(uint16_t minuteOfDay, char* output, size_t outputSize) {
  snprintf(output, outputSize, "%02u:%02u", minuteOfDay / 60U, minuteOfDay % 60U);
}

const char kWebPage[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>教室环境监测器</title
  ><style>
    body {
      max-width: 1400px;
      margin: 24px auto;
      padding: 0 16px;
      font: 16px system-ui;
      color: #172033;
      background: #f5f7fb;
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(340px, 1fr));
      gap: 16px;
      align-items: stretch;
    }
    h1 {
      grid-column: 1 / -1;
      margin: 0;
      font-size: 1.6rem;
    }
    body > #network,
    body > .network-details,
    body > p {
      grid-column: 1 / -1;
    }
    body > #network {
      margin: -8px 0 0;
    }
    .card {
      min-width: 0;
      background: #fff;
      border-radius: 12px;
      padding: 16px;
      margin: 0;
      box-shadow: 0 2px 10px #0001;
    }
    .grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
    }
    .network-details {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 8px;
      margin: -8px 0 0;
    }
    .network-details div {
      min-width: 0;
    }
    .network-details strong {
      display: block;
      margin-top: 2px;
    }
    .value {
      font-size: 1.25rem;
      font-weight: 700;
    }
    label,
    input,
    select,
    textarea,
    button {
      display: block;
      width: 100%;
      box-sizing: border-box;
      margin: 8px 0;
    }
    input,
    select,
    textarea,
    button {
      padding: 10px;
      border-radius: 7px;
      border: 1px solid #ccd3df;
    }
    button {
      background: #2563eb;
      color: white;
      border: 0;
      font-weight: 700;
    }
    .muted {
      color: #596579;
      font-size: 0.9rem;
    }
    @media (max-width: 720px) {
      body,
      .grid,
      .network-details {
        grid-template-columns: 1fr;
      }
      body {
        margin: 16px auto;
        gap: 12px;
      }
    }
  </style>
  <body>
    <h1>教室环境监测器</h1>
    <p id="network" class="muted">加载中…</p>
    <div class="network-details">
      <div><span class="muted">最近数据上报</span><strong id="lastDataUpload">--</strong></div>
      <div><span class="muted">最近心跳上报</span><strong id="lastHeartbeatUpload">--</strong></div>
      <div><span class="muted">API 是否连通</span><strong id="apiConnectivity">--</strong></div>
    </div>
    <section class="card">
      <h2>实时数据</h2>
      <div class="grid">
        <div>
          光照 1
          <div id="light" class="value">--</div>
        </div>
        <div>
          光照 2
          <div id="light2" class="value">--</div>
        </div>
        <div>
          空气 AQI（估算）
          <div id="air" class="value">--</div>
          <div id="airDetail" class="muted">预热并建立基线中…</div>
        </div>
        <div>
          温度
          <div id="temperature" class="value">--</div>
        </div>
        <div>
          湿度
          <div id="humidity" class="value">--</div>
        </div>
        <div>
          人体存在
          <div id="presence" class="value">--</div>
        </div>
      </div>
    </section>
    <section class="card">
      <h2>数据上报</h2>
      <p class="muted">设备 ID：<span id="reportDeviceId">--</span></p>
      <form id="reportingForm" onsubmit="saveReporting(event)">
        <label
          >API Base URL<input
            id="reportBaseUrl"
            name="base_url"
            type="url"
            maxlength="240"
            pattern="https?://.*"
            placeholder="https://api.example.com/api"
            required /></label
        ><label
          >设备 Token（可选）<input
            id="reportToken"
            name="token"
            type="password"
            maxlength="512"
            placeholder="已配置时留空可保留" /></label
        ><label
          >数据上报间隔（秒）<input
            id="reportInterval"
            name="report_interval"
            type="number"
            min="1"
            max="86400"
            step="1"
            required /></label
        ><label
          >心跳间隔（秒，0=关闭）<input
            id="heartbeatInterval"
            name="heartbeat_interval"
            type="number"
            min="0"
            max="86400"
            step="1"
            required /></label
        ><button type="submit">保存上报配置</button>
      </form>
      <div class="grid">
        <button type="button" onclick="manualReport()">立即上报当前数据</button
        ><button type="button" onclick="clearReportLogs()">清空回调日志</button>
      </div>
      <p id="reportStatus" class="muted">读取配置中…</p>
      <pre id="reportLogs" class="muted" style="white-space: pre-wrap; max-height: 260px; overflow: auto">
暂无回调日志</pre
      >
    </section>
    <script>
      const unit = { light: "%", light2: "%", air: " AQI", temperature: " °C", humidity: "%" };
      const reportingForm = document.getElementById("reportingForm");
      const dirtyReportFields = new Set();
      let reportFormLoaded = false,
        reportSaving = false;
      let latestReportState = null,
        latestReportReceivedAt = 0;
      reportingForm.addEventListener("input", (e) => dirtyReportFields.add(e.target.id));
      function setReportField(id, value, force) {
        if (force || !dirtyReportFields.has(id)) document.getElementById(id).value = value;
      }
      function val(v, k) {
        return v === null ? "--" : v + unit[k];
      }
      async function refresh() {
        try {
          let s = await (await fetch("/api/status", { cache: "no-store" })).json();
          for (let k of Object.keys(unit)) document.getElementById(k).textContent = val(s[k], k);
          document.getElementById("presence").textContent = s.presence ? "有人" : "无人";
          document.getElementById("airDetail").textContent = s.airReady
            ? "基线 ADC " + s.airBaseline + " · 当前 " + s.airRaw + " / GPIO " + s.airMv + " mV"
            : "预热并建立基线中 · ADC " + s.airRaw + " / GPIO " + s.airMv + " mV";
          document.getElementById("network").textContent = s.provisioning
            ? "配网热点：" + s.apSsid + "（无密码）"
            : s.connected
              ? "设备 IP：" + s.ip
              : "设备 IP：未连接";
        } catch (e) {
          document.getElementById("network").textContent = "读取设备状态失败";
        }
      }
      function reportingStatus(r) {
        if (!r.configured) return "尚未配置";
        if (r.secure && !r.tlsTimeReady) return "已配置，正在同步 HTTPS 证书校验时间";
        if (r.lastHttpCode >= 200 && r.lastHttpCode < 300) return "已配置，上次上报成功";
        if (r.lastHttpCode > 0) return "已配置，上次上报 HTTP " + r.lastHttpCode;
        if (r.lastHttpCode < 0) return "已配置，上次上报网络错误 " + r.lastHttpCode;
        return "已配置，等待上报";
      }
      function relativeUpload(has, timestamp) {
        if (!has || !latestReportState) return "无记录";
        let base = ((latestReportState.uptimeMs - timestamp) >>> 0) / 1000,
          seconds = Math.floor(base + (Date.now() - latestReportReceivedAt) / 1000);
        if (seconds < 5) return "刚刚";
        if (seconds < 60) return seconds + " 秒前";
        if (seconds < 3600) return Math.floor(seconds / 60) + " 分钟前";
        if (seconds < 86400) return Math.floor(seconds / 3600) + " 小时前";
        return Math.floor(seconds / 86400) + " 天前";
      }
      function renderReportSummary() {
        let r = latestReportState;
        if (!r) return;
        document.getElementById("lastDataUpload").textContent = relativeUpload(
          r.hasDataUpload,
          r.lastDataUploadMs,
        );
        document.getElementById("lastHeartbeatUpload").textContent = relativeUpload(
          r.hasHeartbeatUpload,
          r.lastHeartbeatUploadMs,
        );
        let api = "待检测";
        if (!r.configured) api = "未配置";
        else if (!r.wifiConnected) api = "不通（Wi-Fi 未连接）";
        else if (r.apiConnectivity === "reachable") api = "连通";
        else if (r.apiConnectivity === "unreachable") api = "不通";
        document.getElementById("apiConnectivity").textContent = api;
      }
      async function reporting(force = false) {
        let t = document.getElementById("reportStatus");
        try {
          let response = await fetch("/api/reporting", { cache: "no-store" });
          if (!response.ok) throw new Error(response.status);
          let r = await response.json();
          latestReportState = r;
          latestReportReceivedAt = Date.now();
          renderReportSummary();
          document.getElementById("reportDeviceId").textContent = r.deviceId;
          if (!reportFormLoaded || force) {
            setReportField("reportBaseUrl", r.baseUrl, force);
            setReportField("reportInterval", r.reportIntervalSec, force);
            setReportField("heartbeatInterval", r.heartbeatIntervalSec, force);
            reportFormLoaded = true;
            if (force) dirtyReportFields.clear();
          }
          if (!reportSaving) t.textContent = reportingStatus(r);
        } catch (e) {
          if (!reportSaving) t.textContent = "读取上报配置失败";
        }
      }
      async function saveReporting(e) {
        e.preventDefault();
        let t = document.getElementById("reportStatus");
        reportSaving = true;
        try {
          t.textContent = "正在保存…";
          let r = await fetch("/api/reporting", {
            method: "POST",
            body: new URLSearchParams(new FormData(e.target)),
          });
          let j = await r.json();
          if (!r.ok || !j.ok) throw new Error(j.error || r.status);
          document.getElementById("reportToken").value = "";
          await reporting(true);
          t.textContent = "保存成功，联网后将自动上报";
        } catch (e) {
          t.textContent = "保存失败：" + e.message;
        } finally {
          reportSaving = false;
        }
      }
      async function manualReport() {
        let t = document.getElementById("reportStatus");
        try {
          t.textContent = "正在上报当前数据…";
          let r = await fetch("/api/reporting/send", { method: "POST" }),
            j = await r.json();
          t.textContent = r.ok && j.ok ? "手动上报已排队" : "手动上报失败：" + (j.error || r.status);
        } catch (e) {
          t.textContent = "手动上报请求失败";
        }
      }
      async function reportingLogs() {
        try {
          let r = await (await fetch("/api/reporting/logs", { cache: "no-store" })).json(),
            p = document.getElementById("reportLogs");
          if (!r.length) {
            p.textContent = "暂无回调日志";
            return;
          }
          p.textContent = r
            .slice()
            .reverse()
            .map(
              (x) =>
                "启动后 " +
                Math.floor(x.timeMs / 1000) +
                "s [" +
                x.kind +
                "] HTTP " +
                x.code +
                " bytes=" +
                x.requestBytes +
                "\n" +
                x.response,
            )
            .join("\n\n");
        } catch (e) {}
      }
      async function clearReportLogs() {
        await fetch("/api/reporting/logs/clear", { method: "POST" });
        reportingLogs();
      }
      refresh();
      reporting();
      reportingLogs();
      setInterval(refresh, 2000);
      setInterval(renderReportSummary, 1000);
      setInterval(reporting, 10000);
      setInterval(reportingLogs, 3000);
    </script>
  </body>
</html>
)HTML";

const char kThresholdPage[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta name="viewport" content="width=device-width,initial-scale=1"><title>阈值修改</title>
<style>body{max-width:760px;margin:24px auto;padding:0 16px;font:16px system-ui;color:#172033;background:#f5f7fb}.card{background:#fff;border-radius:10px;padding:20px;box-shadow:0 2px 10px #0001}.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}label{display:block;font-weight:600;margin:10px 0}input{width:100%;box-sizing:border-box;margin-top:7px}input[type=range]{accent-color:#2563eb}output{float:right;font-variant-numeric:tabular-nums;color:#2563eb}.actions{margin-top:18px}button{width:100%;padding:11px;border:0;border-radius:7px;background:#2563eb;color:#fff;font-weight:700}.muted{color:#596579;font-size:.9rem}@media(max-width:600px){.grid{grid-template-columns:1fr}}</style>
<body><section class="card"><h1>系统阈值修改</h1><p class="muted">拖动滑块调整阈值，保存后立即生效。</p><form id="f">
<div class="grid"><label>室外开灯阈值（光照 2）<input id="outdoor" name="outdoor" type="range" min="0" max="100" step="1"><output id="outdoorV"></output></label>
<label>室内目标光照（光照 1）<input id="hold" name="hold" type="range" min="0" max="100" step="1"><output id="holdV"></output></label>
<label>空气 AQI 阈值<input id="aqi" name="aqi" type="range" min="0" max="500" step="1"><output id="aqiV"></output></label>
<label>温度阈值（°C）<input id="temperature" name="temperature" type="range" min="0" max="60" step="0.1"><output id="temperatureV"></output></label>
<label>湿度阈值（%）<input id="humidity" name="humidity" type="range" min="0" max="100" step="1"><output id="humidityV"></output></label>
<label>无人关风扇延时（秒）<input id="fanTimeout" name="fan_timeout" type="range" min="1" max="86400" step="1"><output id="fanTimeoutV"></output></label>
<label>无人关灯延时（秒）<input id="lightingTimeout" name="lighting_timeout" type="range" min="1" max="86400" step="1"><output id="lightingTimeoutV"></output></label>
</div><div class="grid"><label>自动调光开始<input id="start" name="start" type="time" step="60" required></label><label>自动调光结束<input id="end" name="end" type="time" step="60" required></label></div>
<div class="actions"><button type="submit">保存全部阈值</button></div></form><p id="status" class="muted">读取中…</p></section>
<script>const ids=['outdoor','hold','aqi','temperature','humidity','fanTimeout','lightingTimeout'];const units={outdoor:'%',hold:'%',aqi:' AQI',temperature:' °C',humidity:'%',fanTimeout:' 秒',lightingTimeout:' 秒'};const $=id=>document.getElementById(id);function render(){ids.forEach(id=>$(id+'V').textContent=$(id).value+units[id]);}ids.forEach(id=>$(id).addEventListener('input',render));async function load(){try{let r=await (await fetch('/api/thresholds',{cache:'no-store'})).json();$('outdoor').value=r.outdoor;$('hold').value=r.hold;$('aqi').value=r.aqi;$('temperature').value=r.temperature;$('humidity').value=r.humidity;$('fanTimeout').value=r.fanTimeout;$('lightingTimeout').value=r.lightingTimeout;$('start').value=r.start;$('end').value=r.end;render();}catch(e){$('status').textContent='读取阈值失败';}}$('f').onsubmit=async e=>{e.preventDefault();$('status').textContent='正在保存…';try{let r=await fetch('/api/thresholds',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(e.target))}),j=await r.json();if(!r.ok||!j.ok)throw Error(j.message||r.status);$('status').textContent=j.message||'阈值已保存';}catch(e){$('status').textContent='保存失败：'+e.message;}};load();</script></body></html>
)HTML";

const char kNetworkPage[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta name="viewport" content="width=device-width,initial-scale=1"><title>网络连接</title>
<style>body{max-width:620px;margin:24px auto;padding:0 16px;font:16px system-ui;color:#172033;background:#f5f7fb}.card{background:#fff;border-radius:10px;padding:20px;box-shadow:0 2px 10px #0001}label,input,select,button{display:block;width:100%;box-sizing:border-box;margin:10px 0;padding:10px;border-radius:7px;border:1px solid #ccd3df}button{background:#2563eb;color:#fff;border:0;font-weight:700}.muted{color:#596579;font-size:.9rem}</style>
<body><section class="card"><h1>网络连接</h1><p class="muted">扫描附近 Wi‑Fi，选择热点后连接设备。</p><button id="scan" type="button">扫描附近热点</button><p id="scanStatus" class="muted">尚未扫描</p><form method="post" action="/api/wifi"><label>Wi‑Fi 热点<select id="ssidSelect" name="ssid" required><option value="">请先扫描</option><option value="__manual__">手动输入隐藏网络…</option></select></label><input id="ssidManual" maxlength="32" placeholder="输入隐藏网络名称" hidden><label>密码（开放网络可留空）<input name="password" type="password" maxlength="63"></label><button type="submit">连接此热点</button></form><form method="post" action="/api/wifi/reset"><button type="submit">清除已保存网络</button></form></section>
<script>const $=id=>document.getElementById(id);function choose(){let m=$('ssidSelect').value==='__manual__';$('ssidManual').hidden=!m;$('ssidManual').name=m?'ssid':'';$('ssidSelect').name=m?'':'ssid';$('ssidManual').required=m;}$('ssidSelect').onchange=choose;async function scan(start=true){let b=$('scan'),t=$('scanStatus'),s=$('ssidSelect');b.disabled=true;t.textContent='正在扫描附近 Wi‑Fi…';try{let n=await(await fetch('/api/networks'+(start?'?start=1':''),{cache:'no-store'})).json();if(n.scanning){setTimeout(()=>scan(false),800);return;}if(n.error)throw Error(n.error);s.replaceChildren(new Option('请选择 Wi‑Fi',''),new Option('手动输入隐藏网络…','__manual__'));n.networks.forEach(x=>s.add(new Option(x,x)));t.textContent='找到 '+n.networks.length+' 个热点';}catch(e){t.textContent='扫描失败，请重试';}b.disabled=false;b.textContent='重新扫描';}$('scan').onclick=()=>scan(true);</script></body></html>
)HTML";



// The date/time controls avoid browser-specific datetime-local serialization.
const char kTimePageV2[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
  <meta name="viewport" content="width=device-width,initial-scale=1" /><title>RTC 校时</title
  ><style>
    body {
      max-width: 560px;
      margin: 24px auto;
      padding: 0 16px;
      font: 16px system-ui;
      color: #172033;
      background: #f5f7fb;
    }
    .card {
      background: #fff;
      border-radius: 12px;
      padding: 20px;
      box-shadow: 0 2px 12px #0001;
    }
    h1 {
      margin: 0 0 6px;
      font-size: 1.45rem;
    }
    .muted {
      color: #596579;
      font-size: 0.9rem;
    }
    .readout {
      margin: 16px 0;
      padding: 14px;
      border: 1px solid #dbe2ed;
      border-radius: 10px;
      background: #f8fafc;
    }
    .readout strong {
      display: block;
      margin-top: 4px;
      font-size: 1.2rem;
      word-break: break-word;
    }
    .fields {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 12px;
    }
    label {
      display: block;
      font-weight: 600;
    }
    input,
    button {
      display: block;
      width: 100%;
      box-sizing: border-box;
      margin-top: 7px;
      padding: 11px 12px;
      border-radius: 8px;
      border: 1px solid #cbd5e1;
      background: #fff;
      font: inherit;
    }
    button {
      border: 0;
      background: #2563eb;
      color: #fff;
      font-weight: 700;
      cursor: pointer;
    }
    button.secondary {
      background: #0f766e;
    }
    button:disabled {
      opacity: 0.6;
      cursor: wait;
    }
    .actions {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
      margin-top: 16px;
    }
    @media (max-width: 480px) {
      .fields,
      .actions {
        grid-template-columns: 1fr;
      }
    }
  </style>
  <body>
    <section class="card">
      <h1>RTC 时间校准</h1>
      <p class="muted">所有时间均按上海时间（UTC+8）写入 PCF8563T。</p>
      <div class="readout">
        <span class="muted">当前 RTC 读取时间（Asia/Shanghai）</span><strong id="now">--</strong>
      </div>
      <form id="f">
        <label>NTP 服务器<input id="ntp" name="ntp_server" maxlength="128" required /></label>
        <div class="fields">
          <label>日期<input id="date" name="date" type="date" required /></label
          ><label>时间<input id="time" name="time" type="time" step="1" required /></label>
        </div>
        <div class="actions">
          <button id="manual" type="submit">保存并手动校时</button
          ><button id="sync" class="secondary" type="button">立即 NTP 校时</button>
        </div>
      </form>
      <p id="msg" class="muted">读取中…</p>
    </section>
    <script>
      const $ = (id) => document.getElementById(id);
      function fill(v) {
        if (!v) return;
        let x = v.replace("Z", "");
        $("date").value = x.slice(0, 10);
        $("time").value = x.slice(11, 19);
      }
      async function load() {
        try {
          let r = await (await fetch("/api/time", { cache: "no-store" })).json();
          $("now").textContent = r.now || "RTC 无有效读数";
          $("ntp").value = r.ntpServer || "pool.ntp.org";
          if (document.activeElement !== $("date") && document.activeElement !== $("time")) fill(r.now);
          $("msg").textContent = r.ntpPending
            ? "正在等待 NTP 服务器响应…"
            : r.valid
              ? "RTC 已连接"
              : "请检查 PCF8563T 接线";
        } catch (e) {
          $("msg").textContent = "读取时间失败";
        }
      }
      async function post(data, button) {
        button.disabled = true;
        try {
          let r = await fetch("/api/time", {
              method: "POST",
              headers: { "Content-Type": "application/x-www-form-urlencoded" },
              body: new URLSearchParams(data),
            }),
            j = await r.json();
          $("msg").textContent = j.message || "已处理";
          if (!r.ok || !j.ok) throw new Error(j.message || "请求失败");
          await load();
        } catch (e) {
          $("msg").textContent = e.message || "请求失败";
        } finally {
          button.disabled = false;
        }
      }
      $("f").onsubmit = (e) => {
        e.preventDefault();
        post(Object.fromEntries(new FormData(e.target)), $("manual"));
      };
      $("sync").onclick = () => post({ sync: "1", ntp_server: $("ntp").value }, $("sync"));
      load();
      setInterval(load, 2000);
    </script>
  </body>
</html>
)HTML";

const char kHomeTimeWidgetV2[] PROGMEM = R"HTML(
<section class="card rtc-card">
  <h2>时间与 NTP</h2>
  <div class="rtc-readout">
    <span class="muted">RTC 读取时间（上海 UTC+8）</span><strong id="rtcNow">--</strong>
  </div>
  <p id="rtcState" class="muted">读取中…</p>
  <form id="rtcForm">
    <label>NTP 服务器<input id="rtcNtp" name="ntp_server" maxlength="128" required /></label>
    <div class="rtc-fields">
      <label>手动日期<input id="rtcDate" name="date" type="date" required /></label
      ><label>手动时间<input id="rtcTime" name="time" type="time" step="1" required /></label>
    </div>
    <div class="grid">
      <button id="rtcManual" type="submit">保存并手动校时</button
      ><button id="rtcSync" type="button">立即 NTP 校时</button>
    </div>
  </form>
</section>
<style>
  .rtc-card .rtc-readout {
    margin: 12px 0;
    padding: 12px;
    border: 1px solid #dbe2ed;
    border-radius: 9px;
    background: #f8fafc;
  }
  .rtc-card .rtc-readout strong {
    display: block;
    margin-top: 4px;
    font-size: 1.1rem;
    word-break: break-word;
  }
  .rtc-card .rtc-fields {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 10px;
  }
  .rtc-card input {
    background: #fff;
  }
  .rtc-card button:disabled {
    opacity: 0.6;
  }
  @media (max-width: 720px) {
    .rtc-card .rtc-fields {
      grid-template-columns: 1fr;
    }
  }
</style>
<script>
  const rtc$ = (id) => document.getElementById(id);
  function rtcFill(v) {
    if (!v) return;
    let x = v.replace("Z", "");
    rtc$("rtcDate").value = x.slice(0, 10);
    rtc$("rtcTime").value = x.slice(11, 19);
  }
  async function rtcRefresh() {
    try {
      let s = await (await fetch("/api/time", { cache: "no-store" })).json();
      rtc$("rtcNow").textContent = s.now || "无效";
      rtc$("rtcState").textContent = s.ntpPending
        ? "正在等待 NTP 服务器响应…"
        : s.valid
          ? "RTC 正常"
          : "RTC 未读取到，请检查 0x51 接线和供电";
      if (!rtc$("rtcNtp").value) rtc$("rtcNtp").value = s.ntpServer || "pool.ntp.org";
      if (document.activeElement !== rtc$("rtcDate") && document.activeElement !== rtc$("rtcTime"))
        rtcFill(s.now);
    } catch (e) {
      rtc$("rtcState").textContent = "读取 RTC 失败";
    }
  }
  async function rtcPost(data, button) {
    button.disabled = true;
    try {
      let r = await fetch("/api/time", {
          method: "POST",
          headers: { "Content-Type": "application/x-www-form-urlencoded" },
          body: new URLSearchParams(data),
        }),
        j = await r.json();
      rtc$("rtcState").textContent = j.message || "已处理";
      if (!r.ok || !j.ok) throw new Error(j.message || "请求失败");
      await rtcRefresh();
    } catch (e) {
      rtc$("rtcState").textContent = e.message || "请求失败";
    } finally {
      button.disabled = false;
    }
  }
  rtc$("rtcForm").onsubmit = (e) => {
    e.preventDefault();
    rtcPost(Object.fromEntries(new FormData(e.target)), rtc$("rtcManual"));
  };
  rtc$("rtcSync").onclick = () => rtcPost({ sync: "1", ntp_server: rtc$("rtcNtp").value }, rtc$("rtcSync"));
  rtcRefresh();
  setInterval(rtcRefresh, 2000);
</script>
)HTML";

const char kPageNav[] PROGMEM = R"HTML(
<nav class="page-nav" aria-label="页面导航">
  <a href="/">监测首页</a><a href="/thresholds">阈值修改</a><a href="/network">网络连接</a><a href="/actuators">执行器调试</a><a href="/system">系统信息</a>
</nav>
<style>.page-nav{grid-column:1/-1;display:flex;flex-wrap:wrap;gap:8px;margin:0 0 4px}.page-nav a{display:inline-block;padding:9px 13px;border-radius:7px;background:#2563eb;color:#fff;text-decoration:none;font-weight:700}.page-nav a:hover{background:#1d4ed8}</style>
)HTML";

const char kSystemPage[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta name="viewport" content="width=device-width,initial-scale=1" />
<title>系统信息</title><style>
body{max-width:1000px;margin:24px auto;padding:0 16px;font:16px system-ui;color:#172033;background:#f5f7fb}
.card{background:#fff;border-radius:10px;padding:18px;margin-top:14px;box-shadow:0 2px 10px #0001}.page-nav{display:flex;flex-wrap:wrap;gap:8px}.page-nav a{padding:9px 13px;border-radius:7px;background:#2563eb;color:#fff;text-decoration:none;font-weight:700}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:10px}.item{padding:12px;border:1px solid #dbe2ed;border-radius:8px;background:#f8fafc}.muted{color:#596579;font-size:.9rem}strong{display:block;margin-top:4px;font-size:1.12rem;word-break:break-word}button{padding:10px 14px;border:0;border-radius:7px;background:#2563eb;color:#fff;font-weight:700}@media(max-width:600px){body{margin:14px auto}}
</style><body><nav class="page-nav" aria-label="页面导航"><a href="/">监测首页</a><a href="/thresholds">阈值修改</a><a href="/network">网络连接</a><a href="/actuators">执行器调试</a><a href="/system">系统信息</a></nav>
<section class="card"><h1>系统信息</h1><p id="status" class="muted">读取中…</p><div class="grid">
<div class="item">CPU 型号<strong id="chip">--</strong></div><div class="item">CPU 频率<strong id="cpu">--</strong></div><div class="item">CPU ID<strong id="id">--</strong></div>
<div class="item">Flash 总容量<strong id="flash">--</strong></div><div class="item">程序 Flash 占用<strong id="appflash">--</strong></div><div class="item">堆内存<strong id="heap">--</strong></div>
<div class="item">PSRAM 总容量<strong id="psram">--</strong></div><div class="item">PSRAM 已用<strong id="psramused">--</strong></div><div class="item">芯片温度<strong id="temp">--</strong></div>
</div></section><section class="card"><h2>I2C 实时模块列表</h2><p class="muted">以下是本次扫描实际收到 ACK 的 7 位地址；未响应的配置地址不会显示。</p><div class="grid"><div class="item">OLED 总线实时应答<strong id="oled">--</strong></div><div class="item">RTC 总线实时应答<strong id="rtc">--</strong></div></div><p><button id="refresh" type="button">立即重新扫描</button></p></section>
<section class="card"><h2>系统日志</h2><p class="muted">启动诊断和最近硬件状态消息，重启后清空。</p><p><button id="clearLogs" type="button">清空日志</button></p><pre id="logs" class="muted" style="white-space:pre-wrap;max-height:260px;overflow:auto">读取中…</pre></section>
<script>const $=id=>document.getElementById(id), mb=n=>(n/1048576).toFixed(2)+" MB", fmt=a=>a&&a.length?a.join(", "):"未发现设备";async function load(){try{let r=await (await fetch("/api/system-info",{cache:"no-store"})).json();$("chip").textContent=r.chip;$("cpu").textContent=r.cpuMHz+" MHz";$("id").textContent=r.cpuId;$("flash").textContent=mb(r.flashBytes);$("appflash").textContent=mb(r.appBytes)+" / "+mb(r.appPartitionBytes);$("heap").textContent=mb(r.heapUsed)+" / "+mb(r.heapBytes)+"（剩余 "+mb(r.heapFree)+"）";$("psram").textContent=r.psramFound?mb(r.psramBytes):"未检测到";$("psramused").textContent=r.psramFound?mb(r.psramUsed)+"（剩余 "+mb(r.psramFree)+"）":"--";$("temp").textContent=r.temperature===null?"不可用":r.temperature+" °C";$("oled").textContent=fmt(r.oledI2c);$("rtc").textContent=fmt(r.rtcI2c);$("status").textContent="已更新："+new Date().toLocaleTimeString();}catch(e){$("status").textContent="读取系统信息失败"}}async function logs(){try{let a=await (await fetch("/api/system-log",{cache:"no-store"})).json(), box=$("logs");box.textContent=a.length?a.map(x=>x.line).join("\n"):"暂无系统日志";box.scrollTop=box.scrollHeight;}catch(e){$("logs").textContent="读取系统日志失败"}}$("refresh").onclick=()=>{load();logs()};$("clearLogs").onclick=async()=>{await fetch("/api/system-log/clear",{method:"POST"});logs()};load();logs();setInterval(load,5000);setInterval(logs,1000);</script></body></html>
)HTML";




const char kActuatorPageV5[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
  <meta name="viewport" content="width=device-width,initial-scale=1" /><title>手动控制</title
  ><style>

 body {
      max-width: 1200px;
      margin: 24px auto;
      padding: 0 16px;
      font: 16px system-ui;
      color: #172033;
      background: #f5f7fb;
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(340px, 1fr));
      gap: 16px;
      align-items: stretch;
    }
    .card {
      min-width: 0;
      background: #fff;
      border-radius: 12px;
      padding: 20px;
      margin: 0;
      box-shadow: 0 2px 12px #0001;
    }
    h1,
    h2 {
      margin: 0 0 6px;
    }
    .muted {
      color: #596579;
      font-size: 0.9rem;
    }
    .readout {
      margin: 12px 0;
      padding: 12px;
      border: 1px solid #dbe2ed;
      border-radius: 10px;
      background: #f8fafc;
    }
    .readout strong {
      display: block;
      margin-top: 4px;
      font-size: 1.2rem;
    }
    .servo-dial {
      display: block;
      width: min(92vw, 340px);
      height: auto;
      margin: 12px auto;
      touch-action: none;
      user-select: none;
    }
    .servo-dial .track {
      fill: none;
      stroke: #e2e8f0;
      stroke-width: 12;
      stroke-linecap: round;
    }
    .servo-dial .progress {
      fill: none;
      stroke: #2563eb;
      stroke-width: 12;
      stroke-linecap: round;
      stroke-dasharray: 0 1;
    }
    .servo-dial .thumb {
      fill: #2563eb;
      stroke: #fff;
      stroke-width: 4;
    }
    .servo-dial text {
      font: 700 20px system-ui;
      fill: #172033;
      text-anchor: middle;
    }
    .number {
      display: grid;
      grid-template-columns: 1fr 90px;
      gap: 10px;
      align-items: center;
    }
    input,
    button {
      display: block;
      width: 100%;
      box-sizing: border-box;
      margin-top: 10px;
      padding: 11px 12px;
      border-radius: 8px;
      border: 1px solid #cbd5e1;
      background: #fff;
      font: inherit;
    }
    input[type="number"] {
      margin: 0;
    }
    input[type="range"] {
      accent-color: #2563eb;
    }
    button {
      border: 0;
      background: #2563eb;
      color: #fff;
      font-weight: 700;
      cursor: pointer;
    }
    .quick {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 8px;
    }
    .quick button {
      background: #475569;
    }
    .auto {
      background: #0f766e;
    }
    .fan-actions {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 8px;
    }
    .fan-actions button.active {
      background: #0f766e;
    }
    .light-row {
      display: grid;
      grid-template-columns: 110px minmax(0, 1fr) 48px;
      gap: 10px;
      align-items: center;
      margin-top: 12px;
    }
    .light-row input {
      margin: 0;
    }
    .light-row output {
      text-align: right;
      font-variant-numeric: tabular-nums;
    }
    .toggle-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      margin-top: 12px;
    }
    .toggle-row input[type="checkbox"] {
      width: 24px;
      height: 24px;
      margin: 0;
      padding: 0;
      accent-color: #0f766e;
    }
    .status {
      min-height: 24px;
    }
    @media (max-width: 720px) {
      body {
        grid-template-columns: 1fr;
        margin: 16px auto;
        gap: 12px;
      }
      .number,
      .fan-actions,
      .light-row {
        grid-template-columns: 1fr;
      }
      .light-row output {
        text-align: left;
      }
    }
  </style>
  <body>
    <section class="card">
      <h1>舵机</h1>
      <p class="muted">SG90 位置舵机 GPIO21，半圆拖动对应 0°-180°，右端为 0°，向左递增。</p>
      <div class="readout">
        <span class="muted">舵机角度 / 模式</span><strong id="servoCurrent">--</strong>
      </div>
      <svg id="servoDial" class="servo-dial" viewBox="0 0 200 120" aria-label="舵机半圆调试环">
        <path class="track" d="M20 100 C20 0 180 0 180 100" pathLength="1"></path>
        <path id="servoProgress" class="progress" d="M180 100 C180 0 20 0 20 100" pathLength="1"></path>
        <circle id="servoThumb" class="thumb" cx="180" cy="100" r="9"></circle>
        <text id="servoAngle" x="100" y="92">0°</text>
      </svg>
      <div class="number">
        <span class="muted">精确角度</span
        ><input id="servoNumber" type="number" min="0" max="180" step="1" value="0" />
      </div>
      <div class="quick">
        <button type="button" data-angle="0">0°</button><button type="button" data-angle="90">90°</button
        ><button type="button" data-angle="180">180°</button>
      </div>
      <button id="autoServo" class="auto" type="button">恢复自动控制</button>
      <p id="servoStatus" class="muted status"></p>
    </section>
    <section class="card">
      <h2>L9110H 风扇</h2>
      <p class="muted">IN1=GPIO41，IN2=GPIO42。任意一项环境超阈值时自动启动。</p>
      <div class="readout"><span class="muted">风扇状态 / 模式</span><strong id="fanCurrent">--</strong></div>
      <label>速度 <input id="fanSpeed" type="range" min="0" max="100" step="1" value="0" /></label>
      <div class="fan-actions">
        <button id="fanReverse" type="button">反转</button><button id="fanStop" type="button">停止</button
        ><button id="fanForward" type="button">正转</button>
      </div>
      <button id="autoFan" class="auto" type="button">恢复自动控制</button>
      <p id="fanStatus" class="muted status"></p>
    </section>
    <section class="card">
      <h2>LED 灯光</h2>
      <div class="readout"><span class="muted">控制模式</span><strong id="lightingCurrent">--</strong></div>
      <div class="light-row">
        <label for="led1Brightness">LED 1 · GPIO11</label>
        <input id="led1Brightness" type="range" min="0" max="100" step="1" value="0" />
        <output id="led1Value" for="led1Brightness">0%</output>
      </div>
      <div class="light-row">
        <label for="led2Brightness">LED 2 · GPIO12</label>
        <input id="led2Brightness" type="range" min="0" max="100" step="1" value="0" />
        <output id="led2Value" for="led2Brightness">0%</output>
      </div>
      <div class="light-row">
        <label for="led3Brightness">LED 3 · GPIO13</label>
        <input id="led3Brightness" type="range" min="0" max="100" step="1" value="0" />
        <output id="led3Value" for="led3Brightness">0%</output>
      </div>
      <button id="autoLighting" class="auto" type="button">恢复自动调光</button>
      <p id="lightingStatus" class="muted status"></p>
    </section>
    <section class="card">
      <h2>蜂鸣器</h2>
      <div class="readout"><span class="muted">当前状态</span><strong id="buzzerCurrent">--</strong></div>
      <label class="toggle-row" for="buzzerEnabled">
        <span>启用告警声音</span><input id="buzzerEnabled" type="checkbox" />
      </label>
      <p id="buzzerStatus" class="muted status"></p>
    </section>
    <script>
      const $ = (id) => document.getElementById(id),
        svg = $("servoDial"),
        progress = $("servoProgress"),
        thumb = $("servoThumb"),
        servoNumber = $("servoNumber"),
        fanSpeed = $("fanSpeed"),
        lightSliders = [$("led1Brightness"), $("led2Brightness"), $("led3Brightness")],
        buzzerToggle = $("buzzerEnabled"),
        pathLength = progress.getTotalLength();
      let pendingServo = null,
        pendingFan = null,
        pendingLighting = null,
        pendingBuzzer = null,
        pendingAuto = null,
        refreshWanted = false,
        forceLightingRefresh = false,
        busy = false,
        failures = 0,
        fanDirection = 1,
        fanRunning = false,
        lastNonZeroSpeed = 100;
      function servoDraw(angle) {
        angle = Math.max(0, Math.min(180, Math.round(Number(angle) || 0)));
        const ratio = angle / 180,
          point = progress.getPointAtLength(pathLength * ratio);
        progress.style.strokeDasharray = ratio + " 1";
        thumb.setAttribute("cx", point.x.toFixed(2));
        thumb.setAttribute("cy", point.y.toFixed(2));
        $("servoAngle").textContent = angle + "°";
        if (document.activeElement !== servoNumber) servoNumber.value = angle;
      }
      function kick() {
        worker();
      }
      async function worker() {
        if (busy) return;
        busy = true;
        try {
          while (pendingAuto !== null || pendingServo !== null || pendingFan !== null || pendingLighting !== null || pendingBuzzer !== null || refreshWanted) {
            if (pendingAuto !== null) {
              const command = pendingAuto;
              pendingAuto = null;
              await post({ mode: command.mode }, command.statusId, "");
              refreshWanted = true;
              continue;
            }
            if (pendingServo !== null) {
              const angle = pendingServo;
              pendingServo = null;
              await post({ servo_angle: String(angle) }, "servoStatus", "舵机已到 " + angle + "°");
              refreshWanted = true;
              continue;
            }
            if (pendingFan !== null) {
              const command = pendingFan;
              pendingFan = null;
              await post(
                { fan_direction: String(command.direction), fan_speed: String(command.speed) },
                "fanStatus",
                "",
              );
              refreshWanted = true;
              continue;
            }
            if (pendingLighting !== null) {
              const brightness = pendingLighting;
              pendingLighting = null;
              await post(
                {
                  led1_brightness: String(brightness[0]),
                  led2_brightness: String(brightness[1]),
                  led3_brightness: String(brightness[2]),
                },
                "lightingStatus",
                "",
              );
              // POST 完成后设备状态是权威值，即使滑块仍保持焦点也要同步，
              // 避免界面显示值与实际 PWM 脱节。
              forceLightingRefresh = true;
              refreshWanted = true;
              continue;
            }
            if (pendingBuzzer !== null) {
              const enabled = pendingBuzzer;
              pendingBuzzer = null;
              await post(
                { buzzer_enabled: enabled ? "1" : "0" },
                "buzzerStatus",
                enabled ? "蜂鸣器已启用" : "蜂鸣器已停用",
              );
              refreshWanted = true;
              continue;
            }
            refreshWanted = false;
            await refresh();
          }
        } finally {
          busy = false;
        }
      }
      async function post(params, statusId, okText) {
        try {
          const r = await fetch("/api/actuator", {
              method: "POST",
              headers: { "Content-Type": "application/x-www-form-urlencoded" },
              body: new URLSearchParams(params),
            }),
            j = await r.json();
          if (!r.ok || !j.ok) throw new Error(j.message || "HTTP " + r.status);
          $(statusId).textContent = okText || j.message || "已处理";
        } catch (e) {
          $(statusId).textContent = e.message || "控制失败";
        }
      }
      async function refresh() {
        try {
          const r = await fetch("/api/actuator", { cache: "no-store" });
          if (!r.ok) throw new Error("HTTP " + r.status);
          const s = await r.json();
          failures = 0;
          $("servoCurrent").textContent = s.ready
            ? s.angle + "° · " + (s.manual ? "手动" : "自动")
            : "PWM 未就绪";
          if (s.ready && !s.manual && pendingServo === null) servoDraw(s.angle);
          fanDirection = s.fanDirection < 0 ? -1 : 1;
          fanRunning = !!s.fanRunning;
          if (s.fanSpeed > 0) lastNonZeroSpeed = s.fanSpeed;
          $("fanStop").textContent = fanRunning ? "停止" : "开始";
          $("fanCurrent").textContent = s.fanReady
            ? (fanRunning ? (fanDirection > 0 ? "正转 " : "反转 ") + s.fanSpeed + "%" : "已停止") +
              " · " +
              (s.fanManual ? "手动" : "自动")
            : "PWM 未就绪";
          if (document.activeElement !== fanSpeed && pendingFan === null) fanSpeed.value = s.fanSpeed;
          $("lightingCurrent").textContent = s.lightingReady
            ? s.lightingManual
              ? "手动"
              : "自动"
            : "PWM 未就绪";
          const brightness = [s.led1Brightness, s.led2Brightness, s.led3Brightness];
          const syncLighting = forceLightingRefresh ||
                               (pendingLighting === null &&
                                (!s.lightingManual ||
                                 !lightSliders.some((slider) => document.activeElement === slider)));
          lightSliders.forEach((slider, index) => {
            if (syncLighting) slider.value = brightness[index];
            $("led" + (index + 1) + "Value").textContent = slider.value + "%";
          });
          forceLightingRefresh = false;
          $("buzzerCurrent").textContent = s.buzzerEnabled ? "已启用" : "已停用";
          if (document.activeElement !== buzzerToggle && pendingBuzzer === null) {
            buzzerToggle.checked = !!s.buzzerEnabled;
          }
        } catch (e) {
          if (++failures >= 3) {
            $("servoStatus").textContent = "读取状态失败，正在重试";
            $("fanStatus").textContent = "读取状态失败，正在重试";
            $("lightingStatus").textContent = "读取状态失败，正在重试";
            $("buzzerStatus").textContent = "读取状态失败，正在重试";
          }
        }
      }
      function queueServo(angle) {
        servoDraw(angle);
        if (pendingAuto !== null && pendingAuto.mode === "servo_auto") pendingAuto = null;
        pendingServo = Math.max(0, Math.min(180, Math.round(Number(angle) || 0)));
        kick();
      }
      function queueFan(direction) {
        if (pendingAuto !== null && pendingAuto.mode === "fan_auto") pendingAuto = null;
        pendingFan = { direction: direction, speed: direction === 0 ? 0 : Number(fanSpeed.value) };
        kick();
      }
      function queueLighting() {
        if (pendingAuto !== null && pendingAuto.mode === "lighting_auto") pendingAuto = null;
        pendingLighting = lightSliders.map((slider, index) => {
          const value = Math.max(0, Math.min(100, Math.round(Number(slider.value) || 0)));
          $("led" + (index + 1) + "Value").textContent = value + "%";
          return value;
        });
        kick();
      }
      function servoDrag(e) {
        const r = svg.getBoundingClientRect(),
          x = ((e.clientX - r.left) / r.width) * 200,
          y = ((e.clientY - r.top) / r.height) * 120,
          dx = x - 100,
          dy = 100 - y;
        // 角度从右端起算：右端 0°，顶点 90°，左端 180°。
        // 指针滑到中心线以下时 atan2 会绕回负角，直接 clamp 会把端点误判成 0°，
        // 所以这里按左右就近吸附，拖出弧线也不会突然跳值。
        const angle = dy <= 0 ? (x >= 100 ? 0 : 180) : Math.round((Math.atan2(dy, dx) * 180) / Math.PI);
        queueServo(Math.max(0, Math.min(180, angle)));
      }
      svg.onpointerdown = (e) => {
        svg.setPointerCapture(e.pointerId);
        servoDrag(e);
      };
      svg.onpointermove = (e) => {
        if (svg.hasPointerCapture(e.pointerId)) servoDrag(e);
      };
      svg.onpointerup = (e) => svg.releasePointerCapture(e.pointerId);
      servoNumber.onchange = () => queueServo(servoNumber.value);
      document
        .querySelectorAll("[data-angle]")
        .forEach((b) => (b.onclick = () => queueServo(b.dataset.angle)));
      $("fanReverse").onclick = () => {
        fanDirection = -1;
        queueFan(-1);
      };
      $("fanForward").onclick = () => {
        fanDirection = 1;
        queueFan(1);
      };
      $("fanStop").onclick = () => {
        if (fanRunning) {
          queueFan(0);
        } else {
          if (Number(fanSpeed.value) === 0) fanSpeed.value = lastNonZeroSpeed;
          queueFan(fanDirection);
        }
      };
      fanSpeed.oninput = () => queueFan(Number(fanSpeed.value) === 0 ? 0 : fanDirection);
      $("autoServo").onclick = () => {
        pendingServo = null;
        pendingAuto = { mode: "servo_auto", statusId: "servoStatus" };
        kick();
      };
      $("autoFan").onclick = () => {
        pendingFan = null;
        pendingAuto = { mode: "fan_auto", statusId: "fanStatus" };
        kick();
      };
      lightSliders.forEach((slider) => (slider.oninput = queueLighting));
      $("autoLighting").onclick = () => {
        pendingLighting = null;
        pendingAuto = { mode: "lighting_auto", statusId: "lightingStatus" };
        kick();
      };
      buzzerToggle.onchange = () => {
        pendingBuzzer = buzzerToggle.checked;
        kick();
      };
      servoDraw(0);
      setInterval(() => {
        refreshWanted = true;
        kick();
      }, 1000);
      refreshWanted = true;
      kick();
    </script>
  </body>
</html>
)HTML";

const char kWebUiFixes[] PROGMEM = R"HTML(
<script>
  (function () {
    const status = () => document.getElementById("reportStatus"),
      logs = () => document.getElementById("reportLogs");
    window.clearReportLogs = async function () {
      try {
        status().textContent = "正在清空回调日志…";
        let r = await fetch("/api/reporting/logs/clear", {
            method: "POST",
            cache: "no-store",
            headers: { "Content-Type": "application/x-www-form-urlencoded" },
          }),
          j = await r.json();
        if (!r.ok || !j.ok) throw new Error(j.error || r.status);
        logs().textContent = "暂无回调日志";
        status().textContent = "回调日志已清空";
      } catch (e) {
        status().textContent = "清空日志失败：" + e.message;
      }
    };
    window.saveReporting = async function (e) {
      e.preventDefault();
      try {
        status().textContent = "正在保存…";
        let r = await fetch("/api/reporting", {
            method: "POST",
            cache: "no-store",
            headers: { "Content-Type": "application/x-www-form-urlencoded" },
            body: new URLSearchParams(new FormData(e.target)),
          }),
          j = await r.json();
        if (!r.ok || !j.ok) throw new Error(j.error || r.status);
        document.getElementById("reportToken").value = "";
        status().textContent = "保存成功，正在上报";
        setTimeout(() => window.reporting(true), 1100);
      } catch (e) {
        status().textContent = "保存失败：" + e.message;
      }
    };
  })();
</script>
)HTML";


// 数字转 JSON：NaN 输出 "null"，其他按指定小数位数格式化。
String jsonNumber(float value, uint8_t digits) {
  return isnan(value) ? "null" : String(value, static_cast<unsigned int>(digits));
}

// 对字符串做最小 JSON 转义：引号/反斜杠/换行/回车。
String escapeJson(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 4);        // 预留一点余量，减少扩容
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '"' || c == '\\') escaped += '\\';  // 引号 / 反斜杠 前加转义
    if (c == '\n') escaped += "\\n";                // 换行
    else if (c == '\r') escaped += "\\r";           // 回车
    else if (c != '\n') escaped += c;                // 其它字符原样输出
  }
  return escaped;
}
}  // namespace

// 把所有 HTTP 路由注册到内嵌 WebServer。首页动态拼装几段脚本；
// 其它路由委托到本类的成员函数处理，逻辑较重的 /api/actuator 直接就地实现。
void NetworkPortal::configureRoutes() {
  reportUploader_.configureRoutes(server_);
  server_.on("/", HTTP_GET, [this]() {
    String page(kWebPage);
    page.replace("</body>", String(kHomeTimeWidgetV2) + String(kPageNav) + String(kWebUiFixes) + "</body>");
    server_.sendHeader("Cache-Control", "no-store");
    server_.send(200, "text/html; charset=utf-8", page);
  });
  server_.on("/api/status", HTTP_GET, [this]() { sendStatus(); });
  server_.on("/api/lighting-config", HTTP_GET, [this]() { sendLightingConfig(); });
  server_.on("/api/lighting-config", HTTP_POST, [this]() { receiveLightingConfig(); });
  server_.on("/api/thresholds", HTTP_GET, [this]() { sendThresholds(); });
  server_.on("/api/thresholds", HTTP_POST, [this]() { receiveThresholds(); });
  server_.on("/thresholds", HTTP_GET, [this]() {
    server_.sendHeader("Cache-Control", "no-store");
    String page(kThresholdPage);
    page.replace("<body>", String("<body>") + String(kPageNav));
    server_.send(200, "text/html; charset=utf-8", page);
  });
  server_.on("/network", HTTP_GET, [this]() {
    server_.sendHeader("Cache-Control", "no-store");
    String page(kNetworkPage);
    page.replace("<body>", String("<body>") + String(kPageNav));
    server_.send(200, "text/html; charset=utf-8", page);
  });
  server_.on("/actuators", HTTP_GET, [this]() {
    server_.sendHeader("Cache-Control", "no-store");
    String page(kActuatorPageV5);
    page.replace("<body>", String("<body>") + String(kPageNav));
    server_.send(200, "text/html; charset=utf-8", page);
  });
  server_.on("/system", HTTP_GET, [this]() {
    server_.sendHeader("Cache-Control", "no-store");
    server_.send_P(200, "text/html; charset=utf-8", kSystemPage);
  });
  server_.on("/api/actuator", HTTP_GET, [this]() {
    char response[352];
    snprintf(response, sizeof(response),
             "{\"ready\":%s,\"angle\":%u,\"manual\":%s,\"fanReady\":%s,\"fanRunning\":%s,\"fanManual\":%s,\"fanDirection\":%d,\"fanSpeed\":%u,\"lightingReady\":%s,\"lightingManual\":%s,\"led1Brightness\":%u,\"led2Brightness\":%u,\"led3Brightness\":%u,\"buzzerEnabled\":%s}",
             servoReady() ? "true" : "false", static_cast<unsigned int>(servoCurrentAngle()),
             servoManualMode() ? "true" : "false", fanReady() ? "true" : "false",
             fanRunning() ? "true" : "false", fanManualMode() ? "true" : "false",
             static_cast<int>(fanDirection()), static_cast<unsigned int>(fanSpeedPercent()),
             lightingReady() ? "true" : "false", lightingManualMode() ? "true" : "false",
             static_cast<unsigned int>(lightingBrightnessPercent(1)),
             static_cast<unsigned int>(lightingBrightnessPercent(2)),
             static_cast<unsigned int>(lightingBrightnessPercent(3)),
             buzzerEnabled() ? "true" : "false");
    server_.sendHeader("Cache-Control", "no-store");
    server_.send(200, "application/json", response);
  });
  server_.on("/api/actuator", HTTP_POST, [this]() {
    if (server_.hasArg("mode")) {
      const String mode = server_.arg("mode");
      if (mode == "auto" || mode == "servo_auto") servoResumeAutomatic();
      if (mode == "auto" || mode == "fan_auto") fanResumeAutomatic();
      if (mode == "auto" || mode == "lighting_auto") lightingResumeAutomatic();
      if (mode == "auto" || mode == "servo_auto" || mode == "fan_auto" ||
          mode == "lighting_auto") {
        server_.send(200, "application/json", "{\"ok\":true,\"message\":\"已恢复自动控制\"}");
        return;
      }
      server_.send(400, "application/json", "{\"ok\":false,\"message\":\"未知的自动控制模式\"}");
      return;
    }
    bool changed = false;
    String message;
    if (server_.hasArg("angle") || server_.hasArg("servo_angle")) {
      const int angle = (server_.hasArg("servo_angle") ? server_.arg("servo_angle") : server_.arg("angle")).toInt();
      if (angle < 0 || angle > 180) {
        server_.send(400, "application/json", "{\"ok\":false,\"message\":\"舵机角度必须在 0 到 180 之间\"}");
        return;
      }
      if (!servoSetManualAngle(static_cast<uint16_t>(angle))) {
        server_.send(503, "application/json", "{\"ok\":false,\"message\":\"舵机 PWM 尚未就绪\"}");
        return;
      }
      changed = true;
      message = "舵机已转到 " + String(angle) + " 度";
    }
    if (server_.hasArg("fan_direction") || server_.hasArg("fan_speed")) {
      const int requestedDirection = server_.hasArg("fan_direction") ? server_.arg("fan_direction").toInt() : fanDirection();
      const int requestedSpeed = server_.hasArg("fan_speed") ? server_.arg("fan_speed").toInt() : fanSpeedPercent();
      const bool stopCommand = requestedDirection == 0 && requestedSpeed == 0;
      if ((!stopCommand && requestedDirection != -1 && requestedDirection != 1) || requestedSpeed < 0 || requestedSpeed > 100) {
        server_.send(400, "application/json", "{\"ok\":false,\"message\":\"风扇方向必须为 -1/1，速度为 0 到 100\"}");
        return;
      }
      const int8_t direction = stopCommand ? (fanDirection() == 0 ? 1 : fanDirection()) : static_cast<int8_t>(requestedDirection);
      if (!fanSetManual(direction, static_cast<uint8_t>(requestedSpeed))) {
        server_.send(503, "application/json", "{\"ok\":false,\"message\":\"风扇 PWM 尚未就绪\"}");
        return;
      }
      changed = true;
      message += (message.isEmpty() ? "" : "；") + String("风扇") +
                 (requestedSpeed == 0 ? "已停止" : (requestedDirection > 0 ? "正转 " : "反转 ")) +
                 String(requestedSpeed) + "%";
    }
    if (server_.hasArg("led1_brightness") || server_.hasArg("led2_brightness") ||
        server_.hasArg("led3_brightness")) {
      const int led1 = server_.hasArg("led1_brightness")
                           ? server_.arg("led1_brightness").toInt()
                           : lightingBrightnessPercent(1);
      const int led2 = server_.hasArg("led2_brightness")
                           ? server_.arg("led2_brightness").toInt()
                           : lightingBrightnessPercent(2);
      const int led3 = server_.hasArg("led3_brightness")
                           ? server_.arg("led3_brightness").toInt()
                           : lightingBrightnessPercent(3);
      if (led1 < 0 || led1 > 100 || led2 < 0 || led2 > 100 || led3 < 0 || led3 > 100) {
        server_.send(400, "application/json", "{\"ok\":false,\"message\":\"LED 亮度必须在 0 到 100 之间\"}");
        return;
      }
      if (!lightingSetManual(static_cast<uint8_t>(led1), static_cast<uint8_t>(led2),
                             static_cast<uint8_t>(led3))) {
        server_.send(503, "application/json", "{\"ok\":false,\"message\":\"LED PWM 尚未就绪\"}");
        return;
      }
      changed = true;
      message += (message.isEmpty() ? "" : "；") + String("LED 亮度 ") + String(led1) +
                 "/" + String(led2) + "/" + String(led3) + "%";
    }
    if (server_.hasArg("buzzer_enabled")) {
      const String requested = server_.arg("buzzer_enabled");
      if (requested != "0" && requested != "1" && requested != "false" && requested != "true") {
        server_.send(400, "application/json", "{\"ok\":false,\"message\":\"蜂鸣器状态必须为 0 或 1\"}");
        return;
      }
      const bool nextEnabled = requested == "1" || requested == "true";
      if (!buzzerSetEnabled(nextEnabled)) {
        server_.send(500, "application/json", "{\"ok\":false,\"message\":\"蜂鸣器设置写入 NVS 失败\"}");
        return;
      }
      changed = true;
      message += (message.isEmpty() ? "" : "；") +
                 String(nextEnabled ? "蜂鸣器已启用" : "蜂鸣器已停用");
    }
    if (!changed) {
      server_.send(400, "application/json", "{\"ok\":false,\"message\":\"没有可执行的调试参数\"}");
      return;
    }
    server_.send(200, "application/json", String("{\"ok\":true,\"message\":\"") + message + "\"}");
  });
  server_.on("/api/time", HTTP_GET, [this]() { sendTime(); });
  server_.on("/api/time", HTTP_POST, [this]() { receiveTime(); });
  server_.on("/api/networks", HTTP_GET, [this]() { sendNetworks(); });
  server_.on("/api/system-log", HTTP_GET, [this]() { sendSystemLogs(); });
  server_.on("/api/system-log/clear", HTTP_POST, [this]() { clearSystemLogs(); });
  server_.on("/api/system-info", HTTP_GET, [this]() { sendSystemInfo(); });
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

// 从 NVS 读出保存过的 SSID / 密码；SSID 为空视为“还没配过网”。
bool NetworkPortal::loadCredentials(String& ssid, String& password) {
  Preferences preferences;
  if (!preferences.begin(kWifiNamespace, true)) return false;
  ssid = preferences.getString(kSsidKey, "");
  password = preferences.getString(kPasswordKey, "");
  preferences.end();
  return !ssid.isEmpty();
}

// 把 SSID / 密码写入 NVS；putString 返回写入字节数，与字符串长度相同才算成功。
bool NetworkPortal::saveCredentials(const String& ssid, const String& password) {
  Preferences preferences;
  if (!preferences.begin(kWifiNamespace, false)) return false;
  const bool ssidSaved = preferences.putString(kSsidKey, ssid) == ssid.length();
  const bool passwordSaved = preferences.putString(kPasswordKey, password) == password.length();
  preferences.end();
  return ssidSaved && passwordSaved;
}

// 清除 NVS 中所有 Wi-Fi 凭据，用于用户主动“忘记网络”。
void NetworkPortal::clearCredentials() {
  Preferences preferences;
  if (preferences.begin(kWifiNamespace, false)) {
    preferences.clear();
    preferences.end();
  }
}

// 进入配网模式：启动无密码软 AP + captive DNS，OLED 显示热点名和二维码。
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

// 停止 AP/DNS，切换到 STA 模式尝试连接指定 SSID；连接结果由 update() 轮询。
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

// 上电初始化：注册路由、加载凭据，能连就连、连不上就开配网热点。
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

// 由 main 注入阈值对象引用，供 /api/lighting-config 读写。
void NetworkPortal::setThresholds(Thresholds& thresholds) {
  thresholds_ = &thresholds;
}

// GET /api/status - 汇总传感器读数、联网状态、RTC 时间返给 WebUI 首页。
void NetworkPortal::sendStatus() {
  const SensorData emptyData{};
  const SensorData& data = latestData_ == nullptr ? emptyData : *latestData_;
  const bool stationConnected = isStationConnected();
  const String rtcNow = rtcClock.isoString();
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
                    ",\"connected\":" + (stationConnected ? "true" : "false") +
                    ",\"rtcNow\":\"" + rtcNow + "\",\"rtcValid\":" +
                    String(rtcNow.isEmpty() ? "false" : "true") +
                    ",\"ntpServer\":\"" + escapeJson(rtcClock.ntpServer()) + "\"" +
                    ",\"apSsid\":\"" + apSsid_ + "\",\"ip\":\"" +
                    (provisioning_ ? WiFi.softAPIP().toString() :
                     (stationConnected ? WiFi.localIP().toString() : String())) + "\"}";
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", response);
}

// GET /api/lighting-config - 返回照明阈值、时间段及当前自动决策依据。
void NetworkPortal::sendLightingConfig() {
  const Thresholds empty{};
  const Thresholds& thresholds = thresholds_ == nullptr ? empty : *thresholds_;
  const SensorData emptyData{};
  const SensorData& data = latestData_ == nullptr ? emptyData : *latestData_;
  char start[6];
  char end[6];
  formatTimeOfDay(thresholds.lightingStartMinute, start, sizeof(start));
  formatTimeOfDay(thresholds.lightingEndMinute, end, sizeof(end));
  RtcDateTime now;
  const bool rtcValid = rtcClock.read(now);
  const uint16_t minuteOfDay =
      rtcValid ? static_cast<uint16_t>(now.hour * 60U + now.minute) : 0;
  const bool scheduleActive =
      rtcValid && lightingScheduleContainsMinute(thresholds, minuteOfDay);
  const bool outdoorDark = lightingOutdoorDark();
  const bool presenceRecent = lightingPresenceRecent();
  char response[448];
  snprintf(response, sizeof(response),
           "{\"light1\":%.0f,\"light2\":%.0f,\"threshold\":%.0f,\"hold\":%.0f,\"start\":\"%s\",\"end\":\"%s\",\"fanTimeout\":%u,\"lightingTimeout\":%u,\"automatic\":%s,\"rtcValid\":%s,\"scheduleActive\":%s,\"outdoorDark\":%s,\"presenceRecent\":%s}",
           data.light1Percent, data.light2Percent,
           thresholds.outdoorLightThresholdPercent,
           thresholds.lightHoldPercent, start, end,
           static_cast<unsigned>(thresholds.fanNoPresenceTimeoutSeconds),
           static_cast<unsigned>(thresholds.lightingNoPresenceTimeoutSeconds),
           lightingManualMode() ? "false" : "true",
           rtcValid ? "true" : "false", scheduleActive ? "true" : "false",
           outdoorDark ? "true" : "false", presenceRecent ? "true" : "false");
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", response);
}

// POST /api/lighting-config - 校验并保存 WebUI 提交的照明参数。
void NetworkPortal::receiveLightingConfig() {
  if (thresholds_ == nullptr) {
    server_.send(503, "application/json", "{\"ok\":false,\"message\":\"阈值尚未就绪\"}");
    return;
  }
  if (!server_.hasArg("threshold") || !server_.hasArg("hold") ||
      !server_.hasArg("start") || !server_.hasArg("end") ||
      !server_.hasArg("fan_timeout") || !server_.hasArg("lighting_timeout")) {
    server_.send(400, "application/json", "{\"ok\":false,\"message\":\"自动控制设置不完整\"}");
    return;
  }
  const int threshold = server_.arg("threshold").toInt();
  const int hold = server_.arg("hold").toInt();
  if (threshold < 0 || threshold > 100) {
    server_.send(400, "application/json", "{\"ok\":false,\"message\":\"室外光照阈值必须在0到100之间\"}");
    return;
  }
  if (hold < 0 || hold > 100) {
    server_.send(400, "application/json", "{\"ok\":false,\"message\":\"光照保持率必须在0到100之间\"}");
    return;
  }
  const long fanTimeout = server_.arg("fan_timeout").toInt();
  const long lightingTimeout = server_.arg("lighting_timeout").toInt();
  if (fanTimeout < 1 || fanTimeout > 86400) {
    server_.send(400, "application/json", "{\"ok\":false,\"message\":\"无人关风扇延时必须在1到86400秒之间\"}");
    return;
  }
  if (lightingTimeout < 1 || lightingTimeout > 86400) {
    server_.send(400, "application/json", "{\"ok\":false,\"message\":\"无人关灯延时必须在1到86400秒之间\"}");
    return;
  }
  uint16_t start = 0;
  uint16_t end = 0;
  if (!parseTimeOfDay(server_.arg("start"), start) ||
      !parseTimeOfDay(server_.arg("end"), end)) {
    server_.send(400, "application/json", "{\"ok\":false,\"message\":\"时间必须使用24小时制 HH:MM\"}");
    return;
  }
  Thresholds next = *thresholds_;
  next.outdoorLightThresholdPercent = static_cast<float>(threshold);
  next.lightHoldPercent = static_cast<float>(hold);
  next.lightingStartMinute = start;
  next.lightingEndMinute = end;
  next.fanNoPresenceTimeoutSeconds = static_cast<uint32_t>(fanTimeout);
  next.lightingNoPresenceTimeoutSeconds = static_cast<uint32_t>(lightingTimeout);
  if (!thresholdStoreSave(next)) {
    server_.send(500, "application/json", "{\"ok\":false,\"message\":\"自动控制设置保存失败\"}");
    return;
  }
  *thresholds_ = next;
  server_.send(200, "application/json", "{\"ok\":true,\"message\":\"自动控制设置已保存\"}");
}

void NetworkPortal::sendThresholds() {
  const Thresholds empty{};
  const Thresholds& t = thresholds_ == nullptr ? empty : *thresholds_;
  char start[6], end[6];
  formatTimeOfDay(t.lightingStartMinute, start, sizeof(start));
  formatTimeOfDay(t.lightingEndMinute, end, sizeof(end));
  char response[320];
  snprintf(response, sizeof(response),
           "{\"outdoor\":%.0f,\"hold\":%.0f,\"aqi\":%.0f,\"temperature\":%.1f,\"humidity\":%.0f,\"fanTimeout\":%u,\"lightingTimeout\":%u,\"start\":\"%s\",\"end\":\"%s\"}",
           t.outdoorLightThresholdPercent, t.lightHoldPercent, t.mq135Aqi,
           t.temperatureC, t.humidityPercent,
           static_cast<unsigned>(t.fanNoPresenceTimeoutSeconds),
           static_cast<unsigned>(t.lightingNoPresenceTimeoutSeconds), start, end);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", response);
}

void NetworkPortal::receiveThresholds() {
  if (thresholds_ == nullptr) {
    server_.send(503, "application/json", "{\"ok\":false,\"message\":\"阈值尚未就绪\"}");
    return;
  }
  const char* required[] = {"outdoor", "hold", "aqi", "temperature", "humidity",
                            "fan_timeout", "lighting_timeout", "start", "end"};
  for (const char* key : required) {
    if (!server_.hasArg(key)) {
      server_.send(400, "application/json", "{\"ok\":false,\"message\":\"阈值参数不完整\"}");
      return;
    }
  }
  const float outdoor = server_.arg("outdoor").toFloat();
  const float hold = server_.arg("hold").toFloat();
  const float aqi = server_.arg("aqi").toFloat();
  const float temperature = server_.arg("temperature").toFloat();
  const float humidity = server_.arg("humidity").toFloat();
  const long fanTimeout = server_.arg("fan_timeout").toInt();
  const long lightingTimeout = server_.arg("lighting_timeout").toInt();
  uint16_t start = 0, end = 0;
  if (!isfinite(outdoor) || !isfinite(hold) || !isfinite(aqi) || !isfinite(temperature) ||
      !isfinite(humidity) || outdoor < 0 || outdoor > 100 || hold < 0 || hold > 100 ||
      aqi < 0 || aqi > 500 || temperature < 0 || temperature > 60 || humidity < 0 ||
      humidity > 100 || fanTimeout < 1 || fanTimeout > 86400 || lightingTimeout < 1 ||
      lightingTimeout > 86400 || !parseTimeOfDay(server_.arg("start"), start) ||
      !parseTimeOfDay(server_.arg("end"), end)) {
    server_.send(400, "application/json", "{\"ok\":false,\"message\":\"阈值超出允许范围\"}");
    return;
  }
  Thresholds next = *thresholds_;
  next.outdoorLightThresholdPercent = outdoor;
  next.lightHoldPercent = hold;
  next.mq135Aqi = aqi;
  next.temperatureC = temperature;
  next.humidityPercent = humidity;
  next.fanNoPresenceTimeoutSeconds = static_cast<uint32_t>(fanTimeout);
  next.lightingNoPresenceTimeoutSeconds = static_cast<uint32_t>(lightingTimeout);
  next.lightingStartMinute = start;
  next.lightingEndMinute = end;
  if (!thresholdStoreSave(next)) {
    server_.send(500, "application/json", "{\"ok\":false,\"message\":\"阈值保存失败\"}");
    return;
  }
  *thresholds_ = next;
  server_.send(200, "application/json", "{\"ok\":true,\"message\":\"全部阈值已保存\"}");
}

// GET /api/time - 返回 RTC 当前时间、NTP 服务器和同步状态。
// GET /api/time - 返回当前 RTC 时间、NTP 服务器和同步状态给 WebUI。
void NetworkPortal::sendTime() {
  const String now = rtcClock.isoString();
  const String date = now.length() >= 10 ? now.substring(0, 10) : String();
  const String time = now.length() >= 19 ? now.substring(11, 19) : String();
  String response = "{\"now\":\"" + now + "\",\"date\":\"" + date +
                    "\",\"time\":\"" + time + "\",\"valid\":" +
                    String(now.isEmpty() ? "false" : "true") +
                    ",\"ntpServer\":\"" + escapeJson(rtcClock.ntpServer()) +
                    "\",\"ntpPending\":" + String(rtcClock.ntpSyncPending() ? "true" : "false") + "}";
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", response);
}

// POST /api/time - 支持修改 NTP 服务器、手动设置时间或触发一次 NTP 校时。
void NetworkPortal::receiveTime() {
  const String ntpServer = server_.arg("ntp_server");
  if (!ntpServer.isEmpty() && !rtcClock.setNtpServer(ntpServer)) {
    server_.send(400, "application/json", "{\"ok\":false,\"message\":\"NTP 服务器格式或保存失败\"}");
    return;
  }
  String datetime = server_.arg("datetime");
  if (datetime.isEmpty() && server_.hasArg("date") && server_.hasArg("time")) {
    datetime = server_.arg("date") + "T" + server_.arg("time");
  }
  const bool syncRequested = server_.arg("sync") == "1";
  bool manualSet = false;
  if (!datetime.isEmpty()) {
    RtcDateTime value;
    unsigned int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    const int parsed = sscanf(datetime.c_str(), "%u-%u-%uT%u:%u:%u", &year, &month, &day,
                              &hour, &minute, &second);
    if (parsed < 5 || parsed > 6 || year > 65535 || month > 255 || day > 255 ||
        hour > 255 || minute > 255 || second > 255) {
      server_.send(400, "application/json", "{\"ok\":false,\"message\":\"时间格式错误\"}");
      return;
    }
    value.year = static_cast<uint16_t>(year);
    value.month = static_cast<uint8_t>(month);
    value.day = static_cast<uint8_t>(day);
    value.hour = static_cast<uint8_t>(hour);
    value.minute = static_cast<uint8_t>(minute);
    value.second = static_cast<uint8_t>(parsed == 6 ? second : 0);
    manualSet = rtcClock.set(value);
    if (!manualSet) {
      server_.send(400, "application/json", "{\"ok\":false,\"message\":\"RTC 写入失败或日期无效\"}");
      return;
    }
  }
  if (syncRequested) {
    if (WiFi.status() != WL_CONNECTED) {
      server_.send(409, "application/json", "{\"ok\":false,\"message\":\"NTP 配置已保存，Wi-Fi 联网后自动校时\"}");
      return;
    }
    rtcClock.updateNtp(true);
  }
  const char* message = manualSet ? "手动校时完成" :
                        (syncRequested ? "NTP 校时请求已发送" : "NTP 配置已保存");
  server_.send(200, "application/json", String("{\"ok\":true,\"message\":\"") + message + "\"}");
}

// GET /api/networks - 返回上一次扫描的 SSID 列表；start=1 触发新一次异步扫描。
void NetworkPortal::sendNetworks() {
  // 联网后也允许扫描：用户需要在 WebUI 里换到另一个网络。
  // start=1 只由按钮发出；后续轮询不再误触发新扫描或覆盖完成结果。
  if (server_.hasArg("start") && server_.arg("start") == "1" && !networkScanRunning_) {
    networkListJson_ = "{\"networks\":[],\"scanning\":true}";
    networkScanAttempt_ = 0;
    startNetworkScan();
  }
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", networkListJson_);
}

void NetworkPortal::sendSystemLogs() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", SystemLog::json());
}

void NetworkPortal::clearSystemLogs() {
  SystemLog::clear();
  server_.send(200, "application/json", "{\"ok\":true}");
}

void NetworkPortal::sendSystemInfo() {
  const uint32_t heapBytes = ESP.getHeapSize();
  const uint32_t heapFree = ESP.getFreeHeap();
  const bool hasPsram = psramFound();
  const uint32_t psramBytes = hasPsram ? ESP.getPsramSize() : 0;
  const uint32_t psramFree = hasPsram ? ESP.getFreePsram() : 0;
  const uint64_t efuseMac = ESP.getEfuseMac();
  char cpuId[24];
  snprintf(cpuId, sizeof(cpuId), "%012llX", static_cast<unsigned long long>(efuseMac & 0x0000FFFFFFFFFFFFULL));
  String response = "{\"chip\":\"" + escapeJson(ESP.getChipModel()) +
                    "\",\"cpuMHz\":" + String(getCpuFrequencyMhz()) +
                    ",\"cpuId\":\"" + cpuId +
                    "\",\"flashBytes\":" + String(ESP.getFlashChipSize()) +
                    ",\"appBytes\":" + String(ESP.getSketchSize()) +
                    ",\"appPartitionBytes\":" + String(ESP.getSketchSize() + ESP.getFreeSketchSpace()) +
                    ",\"heapBytes\":" + String(heapBytes) +
                    ",\"heapFree\":" + String(heapFree) +
                    ",\"heapUsed\":" + String(heapBytes >= heapFree ? heapBytes - heapFree : 0) +
                    ",\"psramFound\":" + String(hasPsram ? "true" : "false") +
                    ",\"psramBytes\":" + String(psramBytes) +
                    ",\"psramFree\":" + String(psramFree) +
                    ",\"psramUsed\":" + String(psramBytes >= psramFree ? psramBytes - psramFree : 0) +
                    ",\"temperature\":null,\"oledI2c\":" + oledI2cDevicesJson() +
                    ",\"rtcI2c\":" + rtcI2cDevicesJson() + "}";
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", response);
}

// 启动一次异步 Wi-Fi 扫描；失败时按 kNetworkScanMaxAttempts 有限次重试。
bool NetworkPortal::startNetworkScan() {
  if (networkScanRunning_) return false;
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

// AP watchdog：配网期间若热点被外部关闭（例如射频冲突），自动重开。
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

// POST /api/wifi - 接收用户提交的 SSID/密码，校验后延后触发切换到 STA。
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

// 主循环调用：驱动 DNS/HTTP、消费扫描结果、切换连接状态、驱动上报模块。
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

  }

  // 扫描收割必须在 provisioning_ 判断之外：联网后仍要能重新扫描换网，
  // 否则 networkScanRunning_ 永远没人清零，后续扫描全部被拒。
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
    } else if (found == WIFI_SCAN_FAILED ||
               millis() - lastNetworkScanMs_ >= kNetworkScanTimeoutMs) {
      const bool timedOut = found != WIFI_SCAN_FAILED;
      const uint32_t elapsedMs = millis() - lastNetworkScanMs_;
      networkScanRunning_ = false;
      WiFi.scanDelete();
      Serial.printf("[WiFi] scan %s: attempt=%u/%u elapsed=%lums mode=%d stations=%u\r\n",
                    timedOut ? "timed out" : "failed",
                    networkScanAttempt_, kNetworkScanMaxAttempts,
                    static_cast<unsigned long>(elapsedMs), static_cast<int>(WiFi.getMode()),
                    WiFi.softAPgetStationNum());
      if (networkScanAttempt_ < kNetworkScanMaxAttempts) {
        networkListJson_ = "{\"networks\":[],\"scanning\":true}";
        startNetworkScan();
      } else {
        networkListJson_ = timedOut
                               ? "{\"networks\":[],\"scanning\":false,\"error\":\"scan_timeout\"}"
                               : "{\"networks\":[],\"scanning\":false,\"error\":\"scan_failed\"}";
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
  rtcClock.updateNtp(WiFi.status() == WL_CONNECTED);
  reportUploader_.update(latestData);
}

// 配网提示或“连接中”期间返回 true，主页面不应覆盖 OLED。
bool NetworkPortal::isShowingNetworkScreen() const {
  return (provisioning_ && provisioningScreenVisible_) || connecting_;
}

// 只有既不在配网也不在连接过程中、并且 STA 已连成功，才算“已联网”。
bool NetworkPortal::isStationConnected() const {
  return !provisioning_ && !connecting_ && WiFi.status() == WL_CONNECTED;
}

// 已联网时返回 STA 侧 IP 字符串，否则返回空串。
String NetworkPortal::stationIpAddress() const {
  return isStationConnected() ? WiFi.localIP().toString() : String();
}

// 透传数据上报模块的最新状态给 OLED / WebUI。
ReportUploadStatus NetworkPortal::reportUploadStatus() const {
  return reportUploader_.status();
}

// 一次性事件：只在“刚连上路由器”那一轮返回 true，读走后自动清零。
bool NetworkPortal::takeConnectionSucceeded() {
  const bool succeeded = connectionSucceeded_;
  connectionSucceeded_ = false;
  return succeeded;
}
