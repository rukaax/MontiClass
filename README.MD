# MontiClass

[![License: CC BY-NC-SA 4.0](https://img.shields.io/badge/License-CC_BY--NC--SA_4.0-lightgrey.svg)](LICENSE)

基于 **ESP32-S3** 的教室环境监测与自动控制项目，集成环境采集、人体存在检测、智能照明、通风控制和浏览器管理界面。

设备通过 SSD1306 OLED 和 EC11 旋钮提供本地交互，通过 Wi-Fi 提供局域网 WebUI，并可将实时数据上报到外部 HTTP / HTTPS 服务。环境监测和自动控制在设备端运行；完成配置后，日常使用无需连接电脑。

[源码仓库](https://github.com/rukaax/MontiClass) · [问题反馈](https://github.com/rukaax/MontiClass/issues) · [提交改进](https://github.com/rukaax/MontiClass/pulls)

## 目录

- [功能概览](#功能概览)
- [快速开始](#快速开始)
- [硬件与接线](#硬件与接线)
- [构建与部署](#构建与部署)
- [使用与自动控制](#使用与自动控制)
- [数据上报与接口](#数据上报与接口)
- [项目结构](#项目结构)
- [开发与贡献](#开发与贡献)
- [常见问题](#常见问题)
- [许可证](#许可证)

## 功能概览

| 功能 | 说明 |
| --- | --- |
| 环境监测 | 两路光照、MQ135 相对空气质量、DHT11 温湿度、LD2402 人体存在检测 |
| 本地显示与设置 | SSD1306 OLED 显示实时数据，EC11 旋钮调整阈值、查看网络与上报状态 |
| 自动照明 | 根据室外光照、室内目标光照、有人状态和时间段，同步调节三路 LED |
| 自动通风 | L9110H 风扇随环境负荷调速，舵机根据多项环境告警控制开合 |
| 声音告警 | AQI、温度或湿度越过阈值时单次鸣叫，可在 WebUI 停用 |
| Wi-Fi 配网 | 首次启动创建配网热点，支持扫描附近网络和保存连接凭据 |
| WebUI | 提供实时监测、阈值修改、执行器调试、时间设置和系统信息页面，适配手机与电脑 |
| 时间管理 | PCF8563T RTC 保存时间，联网后通过 NTP 校时，统一使用上海时区 |
| 数据上报 | 支持 HTTP / HTTPS、可选 Bearer Token、独立的数据与心跳周期，以及手动上报 |
| 配置持久化 | Wi-Fi、阈值、上报配置、NTP 服务器和蜂鸣器开关分别保存到本地 NVS |
| 无硬件演示 | Python 标准库启动 WebUI 演示，直接复用固件中的页面 |

> MQ135 显示的 `AQI` 是基于本次开机基线的相对估算值，用于趋势观察和本机告警，不等同于国家标准 AQI，也不代表经标定的气体 ppm 浓度。

## 快速开始

克隆项目：

```bash
git clone git@github.com:rukaax/MontiClass.git
cd MontiClass
```

SSH 克隆需要先配置 GitHub SSH Key；也可以使用 HTTPS 地址 `https://github.com/rukaax/MontiClass.git`。


### 部署到客户端

安装 PlatformIO 并按[接线说明](#硬件与接线)连接硬件后，在项目根目录执行：

```bash
pio run -e esp32s3
pio run -e esp32s3 -t upload
pio device monitor -e esp32s3 -b 115200
```

首次启动连接设备热点 **`RX-Classroom-demo`**，访问 <http://192.168.4.1/> 完成配网。构建环境、指定串口和首次验收步骤见[构建与部署](#构建与部署)。

## 硬件与接线

### 硬件清单

| 硬件 | 数量 | 用途 / 要求 |
| --- | --- | --- |
| ESP32-S3 开发板 | 1 | 当前固件按 16 MB Flash、OPI PSRAM 配置，板型为 `esp32-s3-devkitc-1` |
| SSD1306 I²C OLED | 1 | 128 × 64，默认 7 位地址 `0x3C` |
| EC11 旋转编码器 | 1 | 带按键，当前接法按下时 SW 为高电平 |
| 模拟光敏模块 | 2 | 光敏 1 安装在室内，光敏 2 安装在室外 |
| MQ135 模块 | 1 | 空气变化趋势监测，AO 需要分压 |
| DHT11 | 1 | 温度与相对湿度采集 |
| LD2402 人体存在雷达 | 1 | 使用数字 OUT 输出，高电平表示有人 |
| PCF8563T RTC | 1 | 独立 I²C 总线，默认 7 位地址 `0x51` |
| SG90 舵机或兼容舵机 | 1 | 通风机构开合，使用独立 5 V 电源 |
| L9110H 驱动模块及直流风扇 | 1 组 | 风扇 PWM 调速与手动正反转 |
| CJ3400 NMOS 及 LED 支路 | 3 组 | 低侧开关调光，每路 LED 配置合适的限流电阻或恒流驱动 |
| 电源、电阻与连接线 | 若干 | 包括 MQ135 分压、MOSFET 栅极电阻和下拉电阻 |

### GPIO 对照

以下引脚与 `include/` 中各模块的 `Pins` 定义一致。

| 模块 | 信号 | ESP32-S3 GPIO / 连接方式 |
| --- | --- | --- |
| SSD1306 OLED | SDA / SCL | `17` / `18` |
| EC11 | CLK / DT / SW | `4` / `5` / `8` |
| 低电平触发无源蜂鸣器模块 | SIG | `9` |
| 室内光敏 1 | AO | `7` |
| 室外光敏 2 | AO | `6` |
| MQ135 | VCC | `5V` |
| MQ135 | AO | 经 `10 kΩ / 18 kΩ` 分压后接 `15` |
| DHT11 | DATA | `16` |
| LD2402 | OUT | `10`，输入高电平不得超过 3.3 V |
| PCF8563T RTC | SDA / SCL | `47` / `48` |
| 舵机 | PWM | `21` |
| L9110H | IN1 / IN2 | `41` / `42` |
| CJ3400 LED 1 / 2 / 3 | Gate | `11` / `12` / `13` |

### 供电与电平要求

所有模块与 ESP32 必须共地，GPIO 输入电压不得超过 **3.3 V**。

- **MQ135：** 加热器使用 5 V 供电，AO 按 `MQ135 AO → 10 kΩ → GPIO15 → 18 kΩ → GND` 接入；5 V 输入经此分压后约为 3.21 V，禁止将 AO 直接接入 ESP32。
- **I²C：** OLED 使用 GPIO17/18，RTC 使用独立的 GPIO47/48，两条总线分别接线；SDA / SCL 上拉到 3.3 V。启用 8 MB OPI PSRAM 的板卡不要占用 GPIO35/36/37。
- **舵机与风扇：** 使用符合模块规格的独立电源；舵机使用 5 V，不能由 ESP32 的 3.3 V 引脚供电。风扇电机经 L9110H 驱动。
- **LED：** 每只 CJ3400 的 Source 接 GND，Drain 接 LED 负极；Gate 经约 100 Ω 接 GPIO，并用约 10 kΩ 下拉到 GND。LED 正极通过限流电阻或恒流驱动接合适电源。
- **蜂鸣器：** 当前驱动面向低电平触发的无源模块，发声频率为 2.4 kHz，停止时 SIG 保持高电平。裸蜂鸣器或大电流模块需要额外驱动，不能直接从 GPIO 取电。
- **光敏与雷达：** 检查 AO / OUT 的实际电压；5 V 输出必须先分压或转换电平。当前光敏换算按“ADC 越大、环境越暗”配置，更换相反特性的模块时需调整 `src/main.cpp` 中对应的 `kLight*DarkIsHigh` 常量。

当前 LED 使用 20 kHz、10 位 PWM，并在底层将最大占空比限制在约 50%（约 `512/1023`）。WebUI 的 `100%` 表示此允许范围内的最大亮度。5 V LED 电源下支路平均电压约为 2.5 V，但脉冲高电平仍为 5 V，PWM 不能替代电流限制。

## 构建与部署

### 1. 准备开发环境

安装 Git，以及以下任意一种 PlatformIO 环境：

- [VS Code + PlatformIO IDE](https://docs.platformio.org/en/latest/integration/ide/vscode.html)：打开项目根目录，在 PlatformIO 中选择 `esp32s3` 环境的 Build、Upload 和 Monitor 任务。
- [PlatformIO Core CLI](https://docs.platformio.org/en/latest/core/installation/index.html)：安装后在项目根目录执行本文的 `pio` 命令。

确认终端能找到命令：

```bash
git --version
pio --version
```

Python 3 仅用于无硬件 WebUI 演示。固件依赖由 PlatformIO 自动下载，首次构建需要访问软件包和库源。

### 2. 核对开发板配置

构建配置以 [platformio.ini](platformio.ini) 为准：

| 配置项 | 当前值 |
| --- | --- |
| 构建环境 | `esp32s3` |
| 开发平台 / 框架 | `espressif32` / `arduino` |
| 开发板 ID | `esp32-s3-devkitc-1` |
| Flash 大小 / 分区表 | `16MB` / `default_16MB.csv` |
| 存储器类型 | `qio_opi`，并定义 `BOARD_HAS_PSRAM` |
| 串口波特率 | `115200` |
| 自动安装的库 | `U8g2`、`DHT sensor library`、`Adafruit Unified Sensor` |
| 嵌入资源 | `data/cert/x509_crt_bundle.bin`，用于 HTTPS 根证书校验 |

使用不同 Flash 容量或 PSRAM 类型的开发板时，先调整分区表、存储器类型及相关构建标志，再进行烧录。

WebUI 的 HTML / CSS / JavaScript 内嵌在固件源码中，CA 包通过 `board_build.embed_files` 编入固件；部署只需构建并上传固件，无需执行 `uploadfs` 或部署独立前端。

### 3. 编译与烧录

使用 USB 数据线连接开发板的 USB-UART 接口，在项目根目录执行：

```bash
# 编译固件
pio run -e esp32s3

# 查看串口设备
pio device list

# 上传固件，自动识别串口
pio run -e esp32s3 -t upload

# 查看启动与运行日志
pio device monitor -e esp32s3 -b 115200
```

有多个串口时显式指定端口，例如 Linux 下的 `/dev/ttyUSB0`：

```bash
pio run -e esp32s3 -t upload --upload-port /dev/ttyUSB0
pio device monitor -e esp32s3 -p /dev/ttyUSB0 -b 115200
```

Windows 将端口替换为实际的 `COM3` 等设备名；macOS 使用 `pio device list` 返回的 `/dev/cu.*` 路径。上传前关闭占用该端口的串口监视器。如无法自动进入下载模式，可按住 BOOT、短按 RESET，再松开 BOOT 后重试。

编译产物位于 `.pio/build/esp32s3/`，其中 `firmware.bin` 为应用固件；初次部署建议使用 PlatformIO 上传，由工具处理引导程序、分区表和应用的写入地址。

### 4. 首次配网

1. 给设备上电；没有已保存的 Wi-Fi，或开机连接失败时，设备创建无密码热点 **`RX-Classroom-demo`**。这是当前固件的固定 SSID。
2. 手机或电脑连接该热点，扫描 OLED 上的二维码，或直接打开 <http://192.168.4.1/>。
3. 进入“网络连接”，扫描附近 Wi-Fi，选择路由器并提交密码；隐藏网络可手动填写 SSID。使用支持 ESP32-S3 的 2.4 GHz Wi-Fi 网络。
4. 设备关闭配网热点并连接路由器；15 秒内未成功会重新开启热点，可再次提交配置。
5. 连接成功后，让浏览器所在设备接入同一局域网，通过串口日志或 OLED 设置页查看设备 IP，再访问 `http://<设备IP>/`。

OLED 配网提示显示约 1 分钟后返回监测页，热点和 WebUI 继续提供服务。Wi-Fi 凭据保存在设备本地 NVS，不参与数据上报；WebUI 的“清除已保存网络”仅清除网络配置，保留环境阈值等独立设置。

当前固件在开机时尝试一次已保存的 Wi-Fi，运行中断线不会自动反复重连。恢复网络后可重启设备，让它重新尝试连接。

### 5. 完成首次配置与验收

1. 在首页“时间与 NTP”区域检查 RTC 是否有效，按需要设置 NTP 服务器或手动校时；系统固定使用 `Asia/Shanghai`（UTC+8）。
2. 在“阈值修改”页设置光照目标、环境上限、自动照明时段和无人关闭延时。
3. 等待 MQ135 完成约 3 分钟预热；最后 60 秒用于建立本次开机基线，期间尽量保持空气洁净稳定。
4. 在“执行器调试”页逐项检查舵机、风扇、LED 与蜂鸣器，检查完成后将各执行器恢复自动控制。
5. 核对 OLED 与 WebUI 的读数，检查“系统信息”中的 Flash / PSRAM 和两条 I²C 总线设备；如需上报，再配置外部 API 并手动发送一次验证。

## 使用与自动控制

### WebUI 页面

| 地址 | 功能 |
| --- | --- |
| `/` | 环境数据、人体状态、RTC / NTP、上报配置与回调状态 |
| `/thresholds` | 光照目标、环境阈值、无人延时与自动照明时段 |
| `/network` | 扫描 Wi-Fi、连接网络、清除已保存凭据 |
| `/actuators` | 舵机角度、风扇方向与转速、三路独立 LED 亮度、蜂鸣器开关 |
| `/system` | 芯片、Flash、堆内存、PSRAM、I²C 设备和系统日志 |

配网热点无密码，设备 WebUI 使用 HTTP，当前没有登录认证。请在受信任的局域网中使用，避免将管理端口直接暴露到公网。数据上报 Token 用于外部 API 鉴权，与本地 WebUI 访问无关。

### OLED 与旋钮

OLED 监测页显示两路光照、相对 AQI、温度和湿度；人体存在状态在 WebUI 中查看。

- 在监测页按住 EC11 达到 **500 ms** 即进入设置，无需等待松手。
- 旋转选择项目，短按进入修改；旋转调整数值，再次短按确认保存。
- 在设置页长按返回监测页。移动到“湿度”后继续向下旋转，可查看 IP、最近数据 / 心跳上报和 API 连通状态；向上旋转返回阈值列表。
- 室外阈值、室内目标和湿度步长为 1 个百分点，AQI 为 5，温度为 0.5 °C。照明时段和无人延时通过 WebUI 设置。

### 固件默认配置

以下为没有已保存配置时的默认值，定义见 [include/thresholds.h](include/thresholds.h)。

| 参数 | 默认值 | 可配置范围 |
| --- | --- | --- |
| 室外开灯阈值（光敏 2） | `50%` | `0–100%` |
| 室内目标光照（光敏 1） | `50%` | `0–100%` |
| 相对 AQI 告警上限 | `100` | `0–500` |
| 温度告警上限 | `30 °C` | `0–60 °C` |
| 湿度告警上限 | `60%` | `0–100%` |
| 自动照明时段 | `18:00–06:00` | 24 小时制，支持跨午夜；起止相同表示全天允许 |
| 无人关风扇延时 | `60 秒` | `1–86400 秒` |
| 无人关灯延时 | `60 秒` | `1–86400 秒` |
| NTP 服务器 | `pool.ntp.org` | 可在首页修改 |
| 蜂鸣器 | 启用 | 在执行器调试页开关，重启后保留 |

阈值保存到 NVS，重启后恢复；存储模块支持从已有旧版阈值格式迁移，补入新增配置的默认值。

### 自动控制规则

| 执行器 | 自动模式行为 |
| --- | --- |
| 三路 LED | RTC 有效、处于允许时段、最近有人且室外光照低于阈值时，按室内目标光照同步调节三路亮度；条件不满足时关闭 |
| 风扇 | 最近有人且 AQI / 温度 / 湿度任一项超限时，根据环境负荷正转调速；负荷归零或无人超时后停止 |
| 舵机 | AQI、温度、湿度中至少两项超限时转到逻辑 `180°`，少于两项时回到逻辑 `0°` |
| 蜂鸣器 | 任一有效的环境告警项由正常跨越到超限时，以 2.4 kHz 鸣叫约 0.5 秒；持续超限不会反复鸣叫，光照与人体状态不参与此告警 |

自动调光每个新光照样本最多调整约 1% 亮度，室内目标上下 2 个百分点内保持；室外阈值采用 1 个百分点退出回差。风扇启动进入 40% 起转速度，随后每 100 ms 最多调整 1 个百分点，距离目标不超过 2 个百分点时保持。灯光和风扇分别计算无人延时，上电后的初次检测也保留对应延时的宽限。

WebUI 可分别为舵机、风扇和灯光启用手动控制，再分别恢复自动；手动灯光支持三路独立亮度，并覆盖自动时段、光照与人体条件。当前舵机为适配反向安装已反转 PWM 端点，界面仍显示逻辑角度；更换安装方向时检查 `src/main.cpp` 中的 `kReverseDirection`。

MQ135 预热完成前不参与 AQI 告警或风扇负荷计算；完成后基线约对应 AQI 50，再按当前 AO 相对基线的变化映射到 `0–500`。新传感器首次使用还应按模块资料完成老化。DHT11 本轮读取无效时不使用旧值驱动环境控制。光照百分比为 ADC 换算后的相对亮度，不是照度计测得的 lux。

## 数据上报与接口

### 配置外部接收服务

在首页“数据上报”区域设置：

| 配置 | 说明 |
| --- | --- |
| API Base URL | 例如 `https://api.example.com/api`，设备向其后追加 `/reports` |
| 设备 Token | 可选，配置后发送 `Authorization: Bearer <Token>` |
| 数据周期 | 默认 `10 秒`，范围 `1–86400 秒` |
| 心跳周期 | 默认 `60 秒`，范围 `0–86400 秒`，`0` 表示关闭 |

上报配置独立保存到 NVS。Token 保存后只返回“是否已配置”，不会通过查询接口返回明文；再次保存时 Token 留空表示保留原值。

设备联网且满足上报条件后发起首次请求，随后按周期上报，也可点击“立即上报当前数据”。数据与心跳均携带当前完整的有效传感器数据并发送到同一地址；两种周期同时到期时只发送一条心跳请求，同时满足本次数据上报。

外部服务需接受如下请求，并以 HTTP `2xx` 表示接收成功：

```http
POST /api/reports HTTP/1.1
Host: api.example.com
Authorization: Bearer <设备Token>
Content-Type: application/json; charset=utf-8
Connection: close
```

请求体示例（各传感器与 RTC 均有效时）：

```json
{
  "device_id": "esp32s3-AABBCCDDEEFF",
  "light1": 62.0,
  "light2": 28.0,
  "light1_adc": 1556,
  "light2_adc": 2948,
  "aqi": 72,
  "temperature": 25.4,
  "humidity": 58.0,
  "presence": true,
  "timestamp": "2026-10-20T18:30:00+08:00"
}
```

`device_id` 由芯片 MAC 自动生成，格式为 `esp32s3-XXXXXXXXXXXX`。`light1` / `light2` 为 `0–100` 的光照百分比，`*_adc` 为 `0–4095` 的原始 ADC 值，`presence` 为布尔值。`aqi` 仅在预热完成且有效时提供，温湿度仅在有效时提供，湿度单位为 `%`；RTC 无效时省略 `timestamp`，接收端应允许这些可选字段缺失。

HTTP 与 HTTPS 均受支持。HTTPS 使用编入固件的[根 CA 包](data/cert/README.md)校验证书链与域名，并等待有效 RTC 时间及进行中的 NTP 同步完成；时间未就绪时延后上报。等待约 20 秒后仍无有效时间，会记录 `-13` 状态并继续重试。部署接收服务时需保证设备可访问 API 与 NTP 服务器。

### 设备本地接口

以下路径由 ESP32 本机提供，用于 WebUI 和局域网集成。GET 返回 JSON，POST 参数按 `application/x-www-form-urlencoded` 提交；外部 `/reports` 上报使用前述 JSON 格式。

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| `GET` | `/api/status` | 实时传感器、网络和 RTC 状态 |
| `GET` / `POST` | `/api/thresholds` | 查询 / 保存完整阈值配置 |
| `GET` / `POST` | `/api/lighting-config` | 查询 / 保存自动照明及无人延时配置 |
| `GET` / `POST` | `/api/actuator` | 查询执行器状态 / 提交手动控制或恢复自动 |
| `GET` / `POST` | `/api/time` | 查询时间 / 手动校时、配置 NTP 或请求同步 |
| `GET` | `/api/networks` | 获取 Wi-Fi 扫描结果；查询参数 `start=1` 发起扫描 |
| `POST` | `/api/wifi` | 保存并连接 Wi-Fi，参数为 `ssid`、`password` |
| `POST` | `/api/wifi/reset` | 清除已保存 Wi-Fi 并重新配网 |
| `GET` | `/api/system-info` | 芯片、存储器和 I²C 设备信息 |
| `GET` | `/api/system-log` | 读取系统内存日志 |
| `POST` | `/api/system-log/clear` | 清空系统内存日志 |
| `GET` / `POST` | `/api/reporting` | 查询 / 保存上报配置与周期 |
| `POST` | `/api/reporting/send` | 请求立即上报当前数据 |
| `GET` | `/api/reporting/logs` | 查看上报回调与错误 |
| `POST` | `/api/reporting/logs/clear` | 清空上报回调日志 |

例如读取状态和触发手动上报（将示例 IP 替换为实际设备 IP）：

```bash
curl http://192.168.1.88/api/status
curl -X POST http://192.168.1.88/api/reporting/send
```

### 从远程 HTTPS 页面访问局域网设备

固件在 STA 成功联网并取得 DHCP 地址后，还会在 443 端口启动 HTTPS API。本地 CA 和服务器证书的 CN 均为 `Siconn-MontiClass`，服务器证书同时包含当前 IPv4 地址的 `subjectAltName`。

设备会把本地 CA、服务器私钥、服务器证书和签发时的 IPv4 地址保存在 NVS：

- 本次启动取得的 IP 与 NVS 中记录的 IP 一致时，直接复用原服务器证书，不再次生成或签发。
- 只有取得的 IP 与上次签发时不同，才使用同一个本地 CA 重新签发并保存服务器证书。
- IP 改变只需要服务器证书重签；客户端已经安装的本地 CA 不需要重新安装。
- 从旧版固件首次升级到当前证书存储格式时，会重新创建一次 CA，因此需要删除旧证书并重新安装新的 `Siconn-MontiClass.crt`。

#### 客户端必须安装的证书

需要安装的是设备提供的**本地根 CA** `Siconn-MontiClass.crt`，不是 HTTPS 服务器证书，也不需要安装任何私钥。只从自己确认过的 MontiClass 设备下载并信任该证书；把自签 CA 加入受信任根存储后，该 CA 签发的证书都会被本机信任。

1. 让客户端与设备连接到同一局域网。
2. 打开 `http://设备IP/tls/ca.crt`，下载 `Siconn-MontiClass.crt`。
3. 按操作系统安装到受信任根证书存储：
   - **Windows**：运行 `certlm.msc`，进入“受信任的根证书颁发机构” → “证书”，导入 `Siconn-MontiClass.crt`。此操作通常需要管理员权限。
   - **macOS**：使用“钥匙串访问”导入到“系统”钥匙串，打开该证书，在“信任”中把 SSL / 使用此证书时设置为“始终信任”。
   - **Ubuntu / Debian**：执行下面的命令，把证书加入系统 CA 存储。
   - **Android**：设置 → 安全和隐私 → 更多安全设置 → 加密与凭据 → 安装证书，选择 CA / WLAN 证书并选中下载的文件；菜单名称可能随厂商不同。
   - **iPhone / iPad**：打开证书并安装描述文件，然后到“设置” → “通用” → “关于本机” → “证书信任设置”，为 `Siconn-MontiClass` 启用完全信任。
4. 完全退出并重新打开浏览器，再访问 `https://设备IP/api/status`。浏览器不应再显示证书不受信任错误。

```bash
sudo install -m 0644 Siconn-MontiClass.crt /usr/local/share/ca-certificates/Siconn-MontiClass.crt
sudo update-ca-certificates
```

可用 OpenSSL 检查证书 CN，并直接验证 HTTPS：

```bash
openssl x509 -in Siconn-MontiClass.crt -noout -subject -issuer -fingerprint -sha256
curl https://设备IP/api/status
```

只打开浏览器的“继续访问不安全页面”通常不足以让跨域 `fetch()` 通过。串口启动日志会显示是复用已有证书，还是因为 IP 变化而签发新证书。远程网页使用 HTTPS 地址调用；首次从公网网页连接局域网设备时，Chrome 还可能弹出“本地网络访问”权限，用户必须允许。

```js
const esp32 = "https://192.168.1.88";

const status = await fetch(`${esp32}/api/status`, {
  cache: "no-store",
  // 新版 Chromium 支持该提示；IP 字面量通常也能自动识别为 local。
  targetAddressSpace: "local",
}).then((response) => response.json());

await fetch(`${esp32}/api/actuator`, {
  method: "POST",
  headers: { "Content-Type": "application/x-www-form-urlencoded" },
  body: new URLSearchParams({ servo_angle: "120" }),
  targetAddressSpace: "local",
});
```

HTTPS 端目前提供 `GET /api/status`、`GET /api/actuator`、`POST /api/actuator`，并返回 CORS 与本地网络预检响应头。80 端口仍保留给首次配网、完整 WebUI 和 CA 下载。

> 安全提示：这些本地控制接口目前没有登录或 API Token。启用跨域访问后，同一浏览器中获准访问本地网络的网页可能尝试调用设备；请只在可信局域网使用，并在对公网部署前增加设备级鉴权。

完整参数及返回字段见 [src/network_portal.cpp](src/network_portal.cpp) 和 [src/report_uploader.cpp](src/report_uploader.cpp)。系统日志和上报回调日志均为内存记录，重启后清空。上报日志总容量为 12 条，优先保留错误，成功回调最多保留 5 条；成功 / 错误响应正文分别最多保留 64 / 256 字节。

## 项目结构

```text
MontiClass/
├── platformio.ini              # 开发板、分区、PSRAM、依赖与嵌入资源配置
├── README.MD                   # 项目入口与部署文档
├── LICENSE                     # CC BY-NC-SA 4.0 完整许可条款
├── include/                    # 模块接口、GPIO 与共享数据模型
│   ├── sensors.h               # 传感器数据与采集接口
│   ├── environment_status.h    # 环境告警与负荷状态
│   ├── thresholds.h            # 阈值、照明时段和无人延时默认值
│   ├── threshold_store.h       # 阈值持久化接口
│   ├── lighting.h              # 三路 LED 接口与引脚
│   ├── fan.h                   # 风扇接口与引脚
│   ├── servo.h                 # 舵机接口与引脚
│   ├── buzzer.h                # 蜂鸣器接口与引脚
│   ├── control.h               # EC11 编码器与按键事件接口
│   ├── SSD1306.h               # OLED 绘制接口与引脚
│   ├── oled_controller.h       # OLED 页面与交互控制接口
│   ├── network_portal.h        # Wi-Fi 配网与本地 WebUI 接口
│   ├── report_uploader.h       # 上报配置、状态与请求接口
│   ├── rtc_clock.h             # RTC、时区与 NTP 校时接口
│   └── system_log.h            # 系统日志接口
├── src/
│   ├── main.cpp                # setup / loop、传感器采集、执行器驱动与自动策略
│   ├── network_portal.cpp      # 配网、DNS 引导、WebUI 页面与本地 HTTP 路由
│   ├── report_uploader.cpp     # HTTP / HTTPS、JSON 上报、Token 与上报日志
│   ├── rtc_clock.cpp           # PCF8563T I²C 读写与 NTP 同步
│   ├── threshold_store.cpp     # 阈值 NVS 校验、保存与旧版配置迁移
│   ├── control.cpp             # EC11 正交解码、按键消抖与长短按事件
│   ├── oled_controller.cpp     # OLED 页面切换、旋钮设置与渲染调度
│   ├── SSD1306.cpp             # OLED 初始化、I²C 探测与具体页面绘制
│   ├── buzzer.cpp              # 非阻塞鸣叫与蜂鸣器开关持久化
│   └── system_log.cpp          # 内存环形日志与 JSON 输出
├── data/
│   └── cert/
│       ├── README.md           # 根证书包来源说明
│       └── x509_crt_bundle.bin # 构建时嵌入的 HTTPS 根证书包
├── lib/                        # PlatformIO 项目私有库目录，目前保留说明文件
└── test/                       # PlatformIO 测试目录，目前保留说明文件
```

设备主循环先采集数据并处理网络与输入，再统一计算 `EnvironmentStatus`，供自动控制、告警和显示共用。传感器以及 LED / 风扇 / 舵机的硬件实现目前集中在 `src/main.cpp`，对应头文件提供接口；修改策略时应从这里阅读，避免在网络或显示模块中另写一套阈值判断。

`.pio/` 为本机构建产物与依赖缓存，不属于业务源码；编辑器目录也不参与设备部署。

## 开发与贡献

建议先阅读 `platformio.ini` 和 `src/main.cpp` 末尾的 `setup()` / `loop()`，再按需要查看网络、显示、时钟和存储模块。

欢迎通过 [Issues](https://github.com/rukaax/MontiClass/issues) 报告问题或讨论功能，通过 [Pull Requests](https://github.com/rukaax/MontiClass/pulls) 提交改进。提交前请：

1. 执行 `pio run -e esp32s3`，确认固件能够编译。
2. 修改 WebUI 时运行 Python 演示，检查手机和桌面布局，以及受影响的交互流程。
3. 涉及 GPIO、供电、阈值或上报协议时同步更新文档；硬件行为请说明使用的板卡、模块及实测结果。
4. 在 PR 中写明改动原因、验证方法和已知限制，避免提交 Wi-Fi 密码、设备 Token 或本机构建产物。

当前 `test/` 目录只有 PlatformIO 说明文件，尚无项目自动化测试用例。Python 演示用于界面预览与交互验证，传感器采集、时序和执行器控制仍需在实际硬件上验证。

## 常见问题

| 现象 | 排查方式 |
| --- | --- |
| 找不到 `pio` 命令 | 检查 PlatformIO Core 是否安装、是否加入 PATH；使用 IDE 时可打开其 PlatformIO Core CLI 终端 |
| 烧录找不到设备或端口被占用 | 检查 USB 数据线、USB-UART 接口与驱动，运行 `pio device list`，关闭串口监视器，必要时手动进入 BOOT 下载模式 |
| Linux 串口权限不足 | 检查当前用户是否具有对应串口设备访问权限；按所用发行版配置串口用户组后重新登录 |
| 无法打开配网页 | 先确认已连接 `RX-Classroom-demo`，再直接访问 `http://192.168.4.1/`；手机提示“无互联网”时保持该 Wi-Fi 连接 |
| 连接路由器失败 | 检查 2.4 GHz 网络、SSID 与密码；等待约 15 秒回到配网热点后重试 |
| OLED 黑屏 | 检查 GPIO17/18、3.3 V 上拉与 `0x3C` 地址；通过串口或系统日志查看 OLED I²C 探测结果 |
| RTC 提示 `no I2C ACK at 0x51` | 检查 3.3 V 供电、共地、GPIO47/48 是否接反或被占用，并检查 SDA / SCL 上拉电阻 |
| 灯光在自动模式下始终关闭 | 检查 RTC 有效性、当前照明时段、室外阈值、室内目标和人体存在延时；可先用手动模式确认线路 |
| MQ135 暂无 AQI 或数值变化明显 | 等待约 3 分钟预热，并检查最后 60 秒的基线环境、分压、供电和原始 ADC；真实 ppm 需要单独标定 R0 与气体曲线 |
| HTTPS 无法上报或出现 `-13` | 检查 RTC、NTP 可达性与校时状态，再检查 API URL、证书链和设备网络；通过上报回调与 `[REPORT]` 串口日志定位问题 |
| 重启或清除 Wi-Fi 后阈值仍保留 | 配置按独立 NVS 命名空间存储，这是预期行为；清除 Wi-Fi 不会重置所有设置 |

串口波特率固定为 `115200`。EC11 和传感器不输出逐次采样 / 按键日志；排查网络、时钟和上报问题时，可结合串口及 WebUI 的系统信息、系统日志和上报回调查看。

## 许可证

本项目采用 [CC BY-NC-SA 4.0](LICENSE)（知识共享署名—非商业性使用—相同方式共享 4.0 国际）许可。分享或改编时须遵守署名、非商业性使用和相同方式共享要求，完整条款见 [LICENSE](LICENSE) 和 [Creative Commons 官方说明](https://creativecommons.org/licenses/by-nc-sa/4.0/deed.zh-hans)。

© 2026 RukaaX Develop
