/** @file network_portal.cpp
 * @brief 无密码热点配网、WebUI 和 Wi-Fi 凭据的本地 NVS 存储。
 */
#include "network_portal.h"

#include <Preferences.h>
#include <WiFi.h>

#include <math.h>

#include "SSD1306.h"
#include "fan.h"
#include "servo.h"

namespace {
constexpr char kWifiNamespace[] = "wifi_config";
constexpr char kSsidKey[] = "ssid";
constexpr char kPasswordKey[] = "password";
constexpr uint32_t kConnectionTimeoutMs = 15000;
// 配网提示页显示 1 分钟，之后 OLED 回到监测页，但热点和 WebUI 保留。
constexpr uint32_t kProvisioningScreenTimeoutMs = 60000;
constexpr uint32_t kApHealthCheckIntervalMs = 5000;
constexpr uint8_t kNetworkScanMaxAttempts = 3;
// scanComplete() 在射频被 AP/STA 抢占时可能长期停在 -1，必须自己兜底，
// 否则 networkScanRunning_ 永久为真，之后所有扫描请求都被拒绝。
constexpr uint32_t kNetworkScanTimeoutMs = 12000;

const char kWebPage[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>教室环境监测器</title
  ><style>
    body {
      max-width: 680px;
      margin: 24px auto;
      padding: 0 16px;
      font: 16px system-ui;
      color: #172033;
      background: #f5f7fb;
    }
    h1 {
      font-size: 1.6rem;
    }
    .card {
      background: #fff;
      border-radius: 12px;
      padding: 16px;
      margin: 12px 0;
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
      margin: 8px 0 16px;
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
    @media (max-width: 520px) {
      .network-details {
        grid-template-columns: 1fr;
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
      <h2>Wi-Fi 配网</h2>
      <p class="muted">
        扫描后从下拉框选择附近网络；隐藏网络可选“手动输入”。保存后设备将关闭配网热点并尝试连接。
      </p>
      <button id="scanButton" type="button" onclick="networks(true)">扫描附近 Wi-Fi</button>
      <p id="scanStatus" class="muted">尚未扫描</p>
      <form method="post" action="/api/wifi">
        <label
          >Wi-Fi 名称（SSID）<select id="ssidSelect" name="ssid" onchange="chooseSsid()">
            <option value="">请先扫描并选择</option>
            <option value="__manual__">手动输入隐藏网络…</option></select
          ><input id="ssidManual" maxlength="32" placeholder="输入隐藏网络名称" hidden /></label
        ><label>密码（开放网络可留空）<input name="password" type="password" maxlength="63" /></label
        ><button type="submit">保存并连接</button>
      </form>
      <form method="post" action="/api/wifi/reset" onsubmit="return confirm('清除已保存的 Wi-Fi？')">
        <button type="submit">清除 Wi-Fi，重新配网</button>
      </form>
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
      function chooseSsid() {
        let s = document.getElementById("ssidSelect"),
          m = document.getElementById("ssidManual"),
          manual = s.value === "__manual__";
        m.hidden = !manual;
        m.name = manual ? "ssid" : "";
        s.name = manual ? "" : "ssid";
        m.required = manual;
      }
      async function networks(start = false) {
        let b = document.getElementById("scanButton"),
          t = document.getElementById("scanStatus"),
          s = document.getElementById("ssidSelect");
        try {
          b.disabled = true;
          b.textContent = "正在扫描…";
          t.textContent = "正在扫描附近 Wi-Fi，请稍候…";
          let n = await (
            await fetch("/api/networks" + (start ? "?start=1" : ""), { cache: "no-store" })
          ).json();
          if (n.scanning) {
            setTimeout(() => networks(false), 800);
            return;
          }
          if (n.error) throw new Error(n.error);
          s.replaceChildren(new Option("请选择 Wi-Fi", ""), new Option("手动输入隐藏网络…", "__manual__"));
          n.networks.forEach((name) => s.add(new Option(name, name)));
          t.textContent = n.networks.length
            ? "扫描完成，找到 " + n.networks.length + " 个网络。"
            : "扫描完成，未找到网络。";
          b.disabled = false;
          b.textContent = "重新扫描";
        } catch (e) {
          t.textContent = "扫描失败，请重试。";
          b.disabled = false;
          b.textContent = "扫描附近 Wi-Fi";
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
      <p class="muted">所有时间均按上海时间（UTC+8）写入 DS3231。</p>
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
              : "请检查 DS3231 接线";
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
  @media (max-width: 520px) {
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
          : "RTC 未读取到，请检查 0x68 接线和供电";
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

const char kActuatorLink[] PROGMEM = R"HTML(
<p class="muted" style="margin-top: 12px"><a href="/actuators">打开调试执行器页面</a></p>
)HTML";




const char kActuatorPageV5[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
  <meta name="viewport" content="width=device-width,initial-scale=1" /><title>手动控制</title
  ><style>

 body {
      max-width: 600px;
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
      margin-bottom: 14px;
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
    .status {
      min-height: 24px;
    }
    @media (max-width: 420px) {
      .number,
      .fan-actions {
        grid-template-columns: 1fr;
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
    <script>
      const $ = (id) => document.getElementById(id),
        svg = $("servoDial"),
        progress = $("servoProgress"),
        thumb = $("servoThumb"),
        servoNumber = $("servoNumber"),
        fanSpeed = $("fanSpeed"),
        pathLength = progress.getTotalLength();
      let pendingServo = null,
        pendingFan = null,
        pendingAuto = false,
        refreshWanted = false,
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
          while (pendingAuto || pendingServo !== null || pendingFan !== null || refreshWanted) {
            if (pendingAuto) {
              pendingAuto = false;
              await post({ mode: "auto" }, "servoStatus", "");
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
        } catch (e) {
          if (++failures >= 3) {
            $("servoStatus").textContent = "读取状态失败，正在重试";
            $("fanStatus").textContent = "读取状态失败，正在重试";
          }
        }
      }
      function queueServo(angle) {
        servoDraw(angle);
        pendingServo = Math.max(0, Math.min(180, Math.round(Number(angle) || 0)));
        kick();
      }
      function queueFan(direction) {
        pendingFan = { direction: direction, speed: direction === 0 ? 0 : Number(fanSpeed.value) };
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
        pendingAuto = true;
        kick();
      };
      $("autoFan").onclick = () => {
        pendingAuto = true;
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
  server_.on("/", HTTP_GET, [this]() {
    String page(kWebPage);
    page.replace("</body>", String(kHomeTimeWidgetV2) + String(kActuatorLink) + String(kWebUiFixes) + "</body>");
    server_.sendHeader("Cache-Control", "no-store");
    server_.send(200, "text/html; charset=utf-8", page);
  });
  server_.on("/api/status", HTTP_GET, [this]() { sendStatus(); });
  server_.on("/time", HTTP_GET, [this]() {
    server_.sendHeader("Cache-Control", "no-store");
    server_.send_P(200, "text/html; charset=utf-8", kTimePageV2);
  });
  server_.on("/actuators", HTTP_GET, [this]() {
    server_.sendHeader("Cache-Control", "no-store");
    server_.send_P(200, "text/html; charset=utf-8", kActuatorPageV5);
  });
  server_.on("/api/actuator", HTTP_GET, [this]() {
    char response[192];
    snprintf(response, sizeof(response),
             "{\"ready\":%s,\"angle\":%u,\"manual\":%s,\"fanReady\":%s,\"fanRunning\":%s,\"fanManual\":%s,\"fanDirection\":%d,\"fanSpeed\":%u}",
             servoReady() ? "true" : "false", static_cast<unsigned int>(servoCurrentAngle()),
             servoManualMode() ? "true" : "false", fanReady() ? "true" : "false",
             fanRunning() ? "true" : "false", fanManualMode() ? "true" : "false",
             static_cast<int>(fanDirection()), static_cast<unsigned int>(fanSpeedPercent()));
    server_.sendHeader("Cache-Control", "no-store");
    server_.send(200, "application/json", response);
  });
  server_.on("/api/actuator", HTTP_POST, [this]() {
    if (server_.arg("mode") == "auto") {
      servoResumeAutomatic();
      fanResumeAutomatic();
      server_.send(200, "application/json", "{\"ok\":true,\"message\":\"已恢复自动控制\"}");
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
    if (!changed) {
      server_.send(400, "application/json", "{\"ok\":false,\"message\":\"没有可执行的调试参数\"}");
      return;
    }
    server_.send(200, "application/json", String("{\"ok\":true,\"message\":\"") + message + "\"}");
  });
  server_.on("/api/time", HTTP_GET, [this]() { sendTime(); });
  server_.on("/api/time", HTTP_POST, [this]() { receiveTime(); });
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

bool NetworkPortal::isShowingNetworkScreen() const {
  return (provisioning_ && provisioningScreenVisible_) || connecting_;
}

bool NetworkPortal::isStationConnected() const {
  return !provisioning_ && !connecting_ && WiFi.status() == WL_CONNECTED;
}

String NetworkPortal::stationIpAddress() const {
  return isStationConnected() ? WiFi.localIP().toString() : String();
}

ReportUploadStatus NetworkPortal::reportUploadStatus() const {
  return reportUploader_.status();
}

bool NetworkPortal::takeConnectionSucceeded() {
  const bool succeeded = connectionSucceeded_;
  connectionSucceeded_ = false;
  return succeeded;
}
