/** @file report_uploader.h
 * @brief 传感器数据的 HTTP 定时上报与 WebUI 持久化配置。
 */
#pragma once  // 头文件保护，防止被重复包含

#include <Arduino.h>          // Arduino 核心库，提供 String / millis 等

#include "sensors.h"          // SensorData 结构体：本模块要上报的数据源

class WebServer;              // 前向声明：只需知道有这么个类，避免拉入整个头

// 上报服务端的可达性状态（给前端展示用）
enum class ApiConnectivity : uint8_t {
  NotConfigured,   // 还没配 Base URL
  Pending,         // 已配置但还没成功过一次
  Reachable,       // 最近一次返回了合法 HTTP 状态码
  Unreachable,     // 最近一次连接/收发失败
};

// 对外暴露的“上报状态快照”，OLED/WebUI 会用它来显示当前上报情况
struct ReportUploadStatus {
  bool hasDataUpload = false;                 // 是否已经成功发送过一次数据上报
  uint32_t lastDataUploadMs = 0;              // 上次数据上报时的 millis()
  bool hasHeartbeatUpload = false;            // 是否已经发过一次心跳
  uint32_t lastHeartbeatUploadMs = 0;         // 上次心跳的 millis()
  ApiConnectivity apiConnectivity = ApiConnectivity::NotConfigured;  // 连接性状态
};

// 定时把传感器数据 POST 到远端 API，并提供 WebUI 配置接口
class ReportUploader {
 public:
  void begin();                               // 上电初始化：生成 deviceId、加载 NVS 配置
  void configureRoutes(WebServer& server);    // 把 /api/reporting/* 路由挂到 WebServer 上
  void update(const SensorData& data);        // 主循环里定期调用，到点就上报
  ReportUploadStatus status() const;          // 返回当前状态快照

 private:
  // ===== 编译期常量 =====
  static constexpr uint32_t kDefaultReportIntervalMs = 10000;    // 默认 10s 上报一次数据
  static constexpr uint32_t kDefaultHeartbeatIntervalMs = 60000; // 默认 60s 上报一次心跳
  static constexpr uint8_t kLogCapacity = 12;                    // 日志环形缓冲总容量
  static constexpr uint8_t kSuccessLogCapacity = 5;              // 其中最多保留几条成功日志

  // 一条日志的字段：什么时间、什么类型、HTTP 状态、返回体片段、请求字节数
  struct LogEntry {
    uint32_t timeMs = 0;      // 记录时 millis()
    int code = 0;             // HTTP 状态码或负数传输错误码
    String kind;              // "report" / "heartbeat" / "manual" / "tls" 等
    String response;          // 服务器返回体（截断）
    size_t requestBytes = 0;  // 本次发出去的请求字节数
  };

  // ===== HTTP 处理器（挂到 WebServer 上的回调） =====
  void sendConfiguration();     // GET  /api/reporting        返回当前配置和状态
  void receiveConfiguration();  // POST /api/reporting        保存新的配置
  void receiveManualReport();   // POST /api/reporting/send   触发一次立即上报
  void sendLogs();              // GET  /api/reporting/logs   返回日志列表
  void clearLogs();             // POST /api/reporting/logs/clear 清空日志

  // ===== 内部工具 =====
  bool loadConfiguration();                              // 从 NVS 读配置
  bool saveConfiguration(const String& baseUrl, const String& token,
                         uint32_t reportIntervalMs, uint32_t heartbeatIntervalMs);  // 写入 NVS
  String buildRequest(const SensorData& data, const String& host, const String& path) const;  // 拼 HTTP 请求文本
  bool ensureTlsTime();                                  // HTTPS 前确保系统时间已同步
  void appendLog(const char* kind, int code, const String& response, size_t requestBytes);  // 追加一条日志
  int upload(const SensorData& data, const char* kind);  // 真正的连线-发送-读响应流程

  // ===== 运行时状态 =====
  WebServer* server_ = nullptr;                          // 挂载的 WebServer 指针（不拥有）
  String baseUrl_;                                       // 上报根 URL，例如 https://x/y
  String token_;                                         // Bearer Token（可空）
  String deviceId_;                                      // 设备 ID，用芯片 MAC 尾数生成
  uint32_t reportIntervalMs_ = kDefaultReportIntervalMs;         // 数据上报周期
  uint32_t heartbeatIntervalMs_ = kDefaultHeartbeatIntervalMs;   // 心跳周期
  uint32_t lastReportMs_ = 0;                            // 上次尝试上报数据时的 millis()
  uint32_t lastHeartbeatMs_ = 0;                         // 上次尝试心跳时的 millis()
  int lastHttpCode_ = 0;                                 // 上一次的 HTTP 状态/错误码
  bool reportAttempted_ = false;                         // 开机后是否已经至少尝试过一次数据上报
  bool heartbeatAttempted_ = false;                      // 开机后是否已经至少尝试过一次心跳
  bool manualRequested_ = false;                         // WebUI 请求了一次手动上报
  bool hasDataUpload_ = false;                           // 是否已经有过一次数据上报（成功/失败都算）
  bool hasHeartbeatUpload_ = false;                      // 是否已经有过一次心跳
  uint32_t lastDataUploadMs_ = 0;                        // 上一次数据上报发生的 millis()
  uint32_t lastHeartbeatUploadMs_ = 0;                   // 上一次心跳发生的 millis()
  bool timeSyncStarted_ = false;                         // 是否已启动 SNTP 时间同步
  bool timeSyncFailureLogged_ = false;                   // 时间同步失败告警是否已记录过一次（避免刷屏）
  bool timeSyncCompleteLogged_ = false;                  // 时间同步完成日志是否已打过一次
  uint32_t timeSyncStartedMs_ = 0;                       // 时间同步开始时间点
  uint32_t uploadNotBeforeMs_ = 0;                       // 在此 millis() 之前不发起阻塞式上传
  LogEntry logs_[kLogCapacity];                          // 环形日志缓冲
  uint8_t logCount_ = 0;                                 // 当前有效日志条数
};
