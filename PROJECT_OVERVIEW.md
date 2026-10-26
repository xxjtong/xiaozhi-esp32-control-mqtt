# 项目说明文档（PROJECT OVERVIEW）

> 本文档面向第一次接触本项目的开发者，帮助你在最短时间内理解项目的定位、架构、组成、接口与关键实现。
> 阅读顺序建议：先看「1. 项目简介」和「2. 整体架构」，再按需深入后续章节。

---

## 1. 项目简介

### 1.1 这是什么

本项目是开源 AI 语音助手固件 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的**定制化版本**，运行在 ESP32 系列芯片上，实现完整的语音交互闭环：

```
唤醒词 → 录音上传 → 云端 ASR/LLM/TTS → 播放回复 → 连续对话
```

### 1.2 本版本（fork）的两大定制主线

| 主线 | 内容 | 关键代码/文档 |
|---|---|---|
| **硬件适配** | **目标产品** Waveshare ESP32-S3-Touch-LCD-4B（4" 480×480 方形屏 + IMU/RTC；一期固定 5V 供电，不用电池）；**当前测试** 3.49 V2（3.49" 172×640 条形屏） | [4B](main/boards/waveshare/esp32-s3-touch-lcd-4b/) · [3.49 V2](main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/) |
| **第二路 MQTT 控制平面** | 在官方语音协议之外，新增一条独立的 MQTT 通道，实现远程控制（发文字/图片/主题/音量等）与本地用户行为事件上报 | [main/control_mqtt.cc](main/control_mqtt.cc) · [docs/control-mqtt.md](docs/control-mqtt.md) |

### 1.3 快速事实卡

| 项 | 值 |
|---|---|
| 语言/框架 | C/C++，ESP-IDF v6.0.2 |
| 支持芯片 | ESP32 / S2 / S3 / C2 / C3 / C5 / C6 / P4（见 `sdkconfig.defaults.*`） |
| 板级支持 | `main/boards/` 下 46 个板级目录；目标 `waveshare/esp32-s3-touch-lcd-4b`（480×480），当前测试 `waveshare/esp32-s3-touch-lcd-3.49-v2`（172×640） |
| 构建入口 | `python3 scripts/build.py <board> --name <variant>` |
| 主协议服务端 | 小智云（默认 SaaS，OTA 分配 MQTT+UDP/WebSocket；已支持私有化，未来可与企业服务端合并） |
| 控制通道 broker | 第二路 MQTT（独立于主协议，Kconfig 默认 `mqtt.virsical.cn:21883`，可用 NVS 覆盖） |
| 代码仓库 | https://github.com/xxjtong/xiaozhi-esp32-control-mqtt |

---

## 2. 整体架构

### 2.1 分层视图

```
┌─────────────────────────────────────────────────────────────┐
│  应用层   application.cc（主事件循环 / Schedule / 生命周期）      │
│           device_state_machine.cc（设备状态机）                  │
├──────────────────────┬──────────────────────────────────────┤
│  服务层               │                                       │
│  audio/    音频引擎    │  protocols/   主协议（MQTT+UDP / WS） │
│  display/  显示与主题  │  control_mqtt 第二路控制通道（本fork新增）│
│  led/      指示灯     │  mcp_server   设备端 MCP 工具          │
│  notify/   提示音     │  ota.cc        升级                    │
├──────────────────────┴──────────────────────────────────────┤
│  板级抽象层   boards/common/（Board / WifiBoard / 协议编解码等）  │
├─────────────────────────────────────────────────────────────┤
│  板级实现层   main/boards/<vendor>/<board>/（引脚、config.h）    │
├─────────────────────────────────────────────────────────────┤
│  硬件        ESP32-S3 + LCD/触摸 + 音频Codec + IMU + 电池       │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 双通道网络架构（本项目最重要的设计）

设备同时维护**两条相互独立的 MQTT 连接**，职责分离：

| | 主协议通道 | 第二路控制通道（本 fork 新增） |
|---|---|---|
| 用途 | 语音会话：音频流 + 会话 JSON | 远程控制：命令、事件上报、图片、状态查询 |
| 服务端 | xiaozhi.me（OTA 动态分配） | 自有 broker |
| 协议 | MQTT(控制) + UDP(加密音频) 或 WebSocket | 纯 MQTT（JSON 消息） |
| 实现代码 | [main/protocols/](main/protocols/) | [main/control_mqtt.cc](main/control_mqtt.cc) |
| 连接编号 | connect_id=0 | connect_id=1（`NetworkInterface::CreateMqtt(1)`） |
| 生命周期 | 跟随会话开关（`OpenAudioChannel`/`CloseAudioChannel`） | 开机常连，esp-mqtt 自动重连 |

> 设计动机：主协议服务端是第三方（xiaozhi.me）无法扩展；控制平面需要自定义业务（图片下发、事件采集），因此独立建一条常连通道。详见 [docs/control-mqtt.md](docs/control-mqtt.md)。

### 2.3 状态机

设备运行状态由 [main/device_state_machine.cc](main/device_state_machine.cc) 管理，所有状态切换必须走 `Application::SetDeviceState()`。核心状态：

```
Idle（待机） ↔ Connecting ↔ Listening（聆听中） ↔ Speaking（说话中）
                          ↘ Upgrading（升级） / Activating（激活）
```

其他代码可通过 `AddStateChangeListener()` 监听状态变化（本 fork 用它统计交互次数，见 5.4）。

---

## 3. 目录结构

```
xiaozhi-esp32-main/
├── main/                      # 固件主组件
│   ├── main.cc                # 入口：创建 Application 并 Run()
│   ├── application.cc/h       # 主事件循环、协议生命周期、Schedule 调度
│   ├── device_state*.cc/h     # 设备状态机
│   ├── control_mqtt.cc/h      # ★ 第二路 MQTT 控制通道（本 fork 核心增量）
│   ├── audio/                 # 音频服务：采集/播放/Opus/唤醒词/VAD/AEC
│   ├── protocols/             # Protocol 基类 + MqttProtocol + WebsocketProtocol
│   ├── display/               # 显示：Display → LvglDisplay → Lcd/Oled/Emote
│   ├── led/                   # LED 指示
│   ├── notify/                # 提示音播放
│   ├── mcp_server.cc/h        # 设备端 MCP 工具（音量/亮度/主题等，供云端 LLM 调用）
│   ├── ota.cc/h               # OTA 检查与升级（本 fork 已禁用升级执行）
│   ├── settings.cc/h          # NVS 封装
│   ├── system_info.cc/h       # MAC/UUID/版本信息
│   ├── boards/                # 板级实现（46 个目录）
│   │   ├── common/            #   Board 基类、WifiBoard、编解码、显示辅助等
│   │   ├── waveshare/esp32-s3-touch-lcd-4b/      # ★ 目标产品（4" 480×480）
│   │   └── waveshare/esp32-s3-touch-lcd-3.49-v2/  # 当前测试板（172×640）
│   ├── assets/                # 字体、语言包、分区资源
│   └── Kconfig.projbuild      # 板型/功能配置菜单
├── docs/                      # 协议与开发文档（见第 10 节索引）
├── scripts/                   # build.py 构建脚本、host 测试、监控脚本
├── partitions/                # 分区表
├── docker/                    # Docker 构建环境
└── AGENTS.md                  # 面向 AI/开发者的工程规则（必读）
```

---

## 4. 核心组件详解

### 4.1 Application（[main/application.cc](main/application.cc)）

- **单例**，持有主事件循环（FreeRTOS 任务 + 事件组 `event_group_`）。
- **`Schedule(func)`**：把任意任务中的操作投递到主任务执行——这是全项目的线程纪律核心（回调可能来自任意任务，但状态修改必须回到主任务）。
- 管理：协议对象生命周期、音频服务启停、设备状态切换、OTA、MCP 消息分发。
- 本 fork 增量：
  - `ReportUserEvent(event, value)`（L1181 起）：把本地用户行为事件经 `Schedule()` 后由 ControlMqtt 发布；
  - 状态机监听器（L94 起）：每次进入 `Speaking` 记一次 `interaction` 事件（即一轮人机交互）；
  - 唤醒词触发时上报 `wake_word` 事件。

### 4.2 音频子系统（[main/audio/](main/audio/)，另有详细 [main/audio/README.md](main/audio/README.md)）

- `AudioService`：统一调度采集、播放、唤醒词、VAD、Opus 编解码。
- 链路：麦克风 → AFE（AEC/NS/VAD）→ 16k PCM → Opus（60ms VBR + DTX）→ 主协议上传；下行 TTS 音流反向播放。
- 唤醒词：WakeNet 内置模型或自定义唤醒词；本 fork 使用 **“你好，小威”**（见 5.1）。
- 关键约束：**不得阻塞音频任务、不得在音频路径反复大块分配内存**。

### 4.3 协议层（[main/protocols/](main/protocols/)）

- `Protocol` 基类约定共享消息语义（hello/listen/tts/stt/llm/mcp/abort…），协议文档：[docs/mqtt-udp.md](docs/mqtt-udp.md)、[docs/websocket.md](docs/websocket.md)。
- `MqttProtocol`：控制走 MQTT、音频走加密 UDP（默认）；`WebsocketProtocol`：全走 WebSocket 二进制/文本帧。
- 协议地址由 OTA 接口（[main/ota.cc](main/ota.cc)）动态下发，设备启动时请求一次。
- 改共享语义时必须同时验证两种传输。

### 4.4 显示子系统（[main/display/](main/display/)）

- 继承链：`Display`（抽象）→ `LvglDisplay`（LVGL 通用：状态栏/状态文字/通知/弹窗）→ `LcdDisplay`（LCD 屏） / `OledDisplay`。
- 本 fork 增量：
  - `SetStatusColor(rgb)`：状态文字颜色（配合远程 `set_status` 命令，默认颜色随主题自动恢复）；
  - 预览图容器 `preview_viewport_`：全屏透明滚动容器，图片按**屏宽 80%** 等比缩放居中（小图不放大），支持上下拖动，双击清图、三击切主题（[main/display/lcd_display.cc](main/display/lcd_display.cc)）。

### 4.5 板级抽象（[main/boards/common/board.h](main/boards/common/board.h)）

- `Board` 单例接口：`GetNetwork()/GetDisplay()/GetAudioCodec()/GetUuid()/GetBoardJson()` 等。
- 本 fork 新增可选接口：`GetIpAddress() / GetWifiSsid() / GetWifiRssi()`（默认空实现），`WifiBoard` 从 WifiManager 取值，`DualNetworkBoard` 委托当前子板。
- 每个板级目录含：`config.h`（引脚/宏）、板级 `.cc`（唯一 `DECLARE_BOARD(...)` 工厂）、可选 `custom_lcd_display.cc` 等定制类。

### 4.6 MCP 服务端（[main/mcp_server.cc](main/mcp_server.cc)）

设备作为 MCP Server，向云端 LLM 暴露工具（音量、亮度、主题、播报等）；云端经主协议下发 `mcp` JSON-RPC 消息调用。第二路控制通道的 `mcp` 命令可复用该分发逻辑（见 5.3）。

---

## 5. 本 fork 定制实现详解

### 5.1 硬件基线：目标 4B，当前测试 3.49

**目标产品：Waveshare ESP32-S3-Touch-LCD-4B**

| 硬件 | 说明 |
|---|---|
| 屏幕 | 4" 方形 IPS，480×480，RGB ST7701，GT911 电容触摸 |
| 音频 | ES8311（DAC）+ ES7210（ADC），双麦，8Ω 2W 扬声器，`CONFIG_USE_DEVICE_AEC=y` |
| 电源 | USB-C 5V 固定外接；板上 AXP2101 + 锂电接口保留，一期不做电池/电量 UI |
| IMU / RTC | QMI8658 / PCF85063（与 3.49 同型号） |
| 存储 | ESP32-S3R8，16MB Flash，8MB PSRAM |
| 唤醒词 | **“你好，小威”**（`ni hao xiao wei`，自定义 WakeNet/MultiNet 配置） |
| 代码 | [main/boards/waveshare/esp32-s3-touch-lcd-4b/](main/boards/waveshare/esp32-s3-touch-lcd-4b/) |

> 4B 板级目录已存在，但当前使用通用 `LcdDisplay`；fork 的 480×480 工牌 UI、双击/三击手势、公司 logo、IMU/RTC 监控仍需对齐（一期不做电量 UI）。控制通道、设备 MCP、音频链路与板级无关，可先在 3.49 上验证。

**当前测试：Waveshare ESP32-S3-Touch-LCD-3.49 V2**

| 硬件 | 说明 |
|---|---|
| 屏幕 | AXS15231B QSPI，物理 172×640，旋转后可用区域 **640×172** 横条屏 |
| 音频 | ES8311 + ES7210，本板开启 `CONFIG_USE_DEVICE_AEC=y` |
| IO 扩展 | TCA9554（背光/功放使能等） |
| IMU | 运动检测在 **Idle / Notifying** 下触发聆听（`StartListening()` + 设备默认模式）并上报 `motion_detected`；播报中先停播报再进聆听。5 秒冷却；屏幕点击与 BOOT/PWR 按键会打时间戳，其后 700ms 忽略运动（避免点击抖动误触发）。可用 `set_imu` 关闭 |
| 电池 | ADC1_CH3 (GPIO4)，200kΩ/100kΩ 3:1 分压采样 |
| 触摸 | 空闲时：双击清预览图（`image_cleared`），三击切主题（`theme_changed`）；**播报中双击 = 停止播报**（`notify_stopped`），不清图也不切主题 |
| 唤醒词 | **“你好，小威”**（当前测试板配置；4B 已同步同一组配置） |

代码：[main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/](main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/)（含 `custom_lcd_display.cc`、`imu_monitor.h`）。

> 迁移影响：3.49 的 640×172 条形 UI **不能直接复用到 480×480 方形屏**；4B 需要单独设计工牌卡片布局，并在试点前完成手势/IMU/RTC/电量对齐。3.49 继续作为音频、控制通道和固件基线测试板。

### 5.2 控制通道生命周期（[main/control_mqtt.cc](main/control_mqtt.cc)）

- 配置：NVS 命名空间 `control_mqtt`（键：`endpoint/client_id/username/password/subscribe_topic/reply_topic`），缺省回落 Kconfig 默认值；`endpoint` 为空则不启动。
- 主题约定：订阅 `xiaozhi/<device_id>/cmd`，发布 `xiaozhi/<device_id>/reply`（`device_id` 为板级 UUID）。
- 连接：独立 FreeRTOS 任务执行 `Mqtt::Connect()`（超时自控）；失败**不销毁客户端**，交由 esp-mqtt 自动重连（默认开启，约 10s 间隔），重连成功后重新订阅并广播 `hello`。
- JSON 处理全部在主任务执行（回调里 `Schedule()` 投递），遵守单线程纪律。
- 线程安全：`lifecycle_mutex_` 保护连接任务与析构竞争；`image_fetch_running_` 原子量保证图片下载单飞。

### 5.3 控制命令接口（下行：`xiaozhi/<id>/cmd`）

所有命令为 JSON，`type` 字段区分；执行结果经 reply 主题回执 `{"type":"reply","command_type":"<type>","success":true|false,...}`。

| type | 功能 | 主要参数 | 状态约束 |
|---|---|---|---|
| `listen` | 远程开/关聆听 | `state: "start"|"stop"` | start 仅限 Idle/Notifying；stop 任意时刻 |
| `abort` | 中断当前会话/说话 | `reason`（可选） | 任意时刻 |
| `mcp` | 调用设备端 MCP 工具 | MCP JSON-RPC 消息体 | 仅 Idle（避免与主会话冲突） |
| `system` | 系统命令 | `command: "reboot"` 等 | — |
| `set_image` | 下发/清除图片 | `url`（HTTP/JPEG·PNG），或 `clear:true`；可选 `mode: "preview"\|"background"` | preview 浮层常驻 / background 桌面壁纸（cover 裁剪，跨主题保留；支持 JPEG/PNG） |
| `set_theme` | 切换明暗主题 | `name: "dark"|"light"` | — |
| `set_volume` | 设置音量 | `volume: 0-100` | — |
| `set_brightness` | 设置背光 | `brightness: 0-100` | — |
| `toggle_wake_word` | 开关唤醒词 | — | — |
| `notify` | 远程语音播报 | `audio_url`（单声道 Ogg Opus 直链）、`subtitles[]`（可选）、`id`（可选，对账用） | 仅 Idle；回执 `success` 表示**已接受并开始播放** |
| `abort_notify` | 取消正在播报的音频 | — | 仅 Notifying；清空解码/播放队列立即静音，结束后上报 `outcome=cancelled` |
| `set_status` | 状态栏文字（可滚动） | `text`，可选 `color`（色名或 `#RRGGBB`） | — |
| `toggle_power_save` | 切换省电模式 | — | — |
| `set_imu` | 开关 IMU 运动检测 | `enable: true\|false`（缺省 true） | 仅支持带 IMU 的板型；持久化到 NVS `imu/enabled` |
| `query_status` | 查询设备状态 | — | extra 返回 `ip/ssid/rssi/battery/temperature/theme/volume/brightness/preview_active/wallpaper_active/imu_enabled/wake_word_running/notify_active/notify_id`（有值才带） |

**图片链路实现**（`HandleSetImage` → `ImageFetchTaskEntry`）：
1. 下载任务（独立任务，栈 12KB，原子量单飞）；
2. HTTP 头部预检 `Content-Length` > 4MB 直接拒绝（不下载）；
3. JPEG/PNG 头部解析宽高（扫 SOF / IHDR），超过 1280×1280 或 100 万像素拒绝（不分配解码内存）；
4. JPEG 经 `jpeg_to_image` 解为 RGB565，主任务构造 `LvglAllocatedImage`；
5. 显示按 `mode` 分流：`preview`（默认）按 `LV_HOR_RES*80%` 等比缩放（小图不放大超过原尺寸）、垂直居中、可上下拖动，`CancelPreviewTimeout()` 常驻；`background` 经 `CreateCoverBackground()` 做 cover 居中裁剪到屏尺寸，写入 light/dark 主题的 `background_image` 后 `SetTheme` 重绘背景层。JPEG 直接使用解码后的 RGB565；PNG 在显示层用 LVGL lodepng 解为 ARGB8888 后转 RGB565（透明像素叠黑底）。解码/OOM 失败如实回 `success=false`。

### 5.4 上行消息（`xiaozhi/<id>/reply`）

| type | 时机 | 关键字段 |
|---|---|---|
| `hello` | 连接/重连成功后 | `device_id/status/version/ip` |
| `reply` | 每条命令的执行回执 | `command_type/success/device_state/extra/id`（`id` 仅回传 `notify` 的） |
| `goodbye` | 停用/析构前 | — |
| `session_update` | 设备状态变化 | `device_id/status` |
| `notify_result` | 播报离开 `notifying` 状态时（一次） | `device_id/id/outcome/duration_ms`；`outcome` 见 [control-mqtt.md](docs/control-mqtt.md) §5.3 |
| `event` | 本地用户行为 | `device_id/event/value/state/uptime_ms/seq` |

**用户行为事件表**（QoS 0 尽力而为，`seq` 单调递增，只含元数据不含音频/文本内容）：

| event | 触发点 | 采集实现 |
|---|---|---|
| `wake_word` | 唤醒词命中 | `HandleWakeWordDetectedEvent` 入口 |
| `interaction` | 一轮人机交互（进入 Speaking 计一次） | 状态机监听器 `old!=Speaking && new==Speaking` |
| `button_click` | BOOT 按键单击 | 板级 `boot_button_.OnClick` |
| `motion_detected` | IMU 检测到姿态变化（Idle / Notifying 下触发聆听） | `imu_monitor.h` |
| `notify_stopped` | 本地手势停止播报（屏幕双击 / BOOT 键） | `custom_lcd_display.cc`（value=`double_click`） |
| `image_cleared` | 双击清除常驻预览图（仅空闲时） | `custom_lcd_display.cc` |
| `theme_changed` | 三击切换主题 | `custom_lcd_display.cc`，value=新主题名 |

### 5.5 网络信息上报

- `Board` 层新增 `GetIpAddress/GetWifiSsid/GetWifiRssi` 可选接口（[main/boards/common/board.h](main/boards/common/board.h)），`WifiBoard` 实现之；
- `hello` 消息携带 `ip`；`query_status` 的 extra 携带 `ip/ssid/rssi`，并补充 `theme/volume/brightness/preview_active/wallpaper_active/imu_enabled/wake_word_running` 等自测诊断字段。

### 5.6 其他定制

- **OTA 升级执行已禁用**：`application.cc` 中 `UpgradeFirmware` 调用被注释，仅保留 `MarkCurrentVersionValid()`（防止意外自升级，版本检查在配置了 MQTT/WS 时跳过）；
- **自定义唤醒词**：`CONFIG_USE_CUSTOM_WAKE_WORD=y` + 关闭默认 WakeNet 模型 + 使能 MultiNet 模型，唤醒词拼音/阈值在板级 `config.json` 的 `sdkconfig_append`；
- **监控脚本**：[scripts/control_mqtt_monitor.py](scripts/control_mqtt_monitor.py) 订阅控制通道打印消息（paho 1.x/2.x 兼容，`--probe-interval` 可周期查询设备状态）。

---

## 6. 对外接口总览

| 接口面 | 说明 | 文档 |
|---|---|---|
| OTA 接口 | 启动时 POST 设备信息，返回主协议地址（WS/MQTT）与激活配置 | 上游 README |
| 主协议 | 语音会话（hello/listen/tts/mcp/…），MQTT+UDP 或 WebSocket | [docs/mqtt-udp.md](docs/mqtt-udp.md) · [docs/websocket.md](docs/websocket.md) |
| MCP | 云端 LLM ↔ 设备工具调用 | [docs/mcp-protocol.md](docs/mcp-protocol.md) |
| 控制通道 | 远程命令/事件/图片/状态（本 fork） | [docs/control-mqtt.md](docs/control-mqtt.md)（含完整 JSON 示例与图片规格建议） |
| Board 抽象 | 新板接入的 C++ 接口约定 | [docs/custom-board.md](docs/custom-board.md) |

---

## 7. 构建与烧录

### 7.1 环境准备

```sh
# 安装 ESP-IDF v6.0.2 后
source /path/to/esp-idf/export.sh
idf.py --version
```

### 7.2 构建

```sh
python3 scripts/build.py --list-boards                                   # 查看板型名
python3 scripts/build.py waveshare/esp32-s3-touch-lcd-4b \
        --name esp32-s3-touch-lcd-4b                                     # 目标产品（4" 480×480）
python3 scripts/build.py waveshare/esp32-s3-touch-lcd-3.49-v2 \
        --name esp32-s3-touch-lcd-3.49-v2                                # 当前测试/固件基线（172×640）
python3 -m unittest discover -s scripts/tests -v                         # host 侧测试
```

> 板型选择是耦合链路：`config.json` → `scripts/build.py` → `main/Kconfig.projbuild` → `main/CMakeLists.txt` → 板级源码与 `config.h`。新增板型必须打通每一环，且 `DECLARE_BOARD(...)` 全工程唯一（板型标识影响 OTA 兼容性）。

### 7.3 烧录（本 fork 实测路径）

- 串口：`/dev/cu.usbmodem1101`（macOS，注意用 `cu.` 而非 `tty.`）；
- 可直接 `idf.py -p /dev/cu.usbmodem1101 flash monitor`，或在 python 环境不一致时改用 esptool 命令行按 `@flash_args` 烧录；
- **应用分区偏移是 `0x20000`**（不是常见的 `0x10000`），assets 在 `0x800000`；只刷 app 可保留 NVS/WiFi 配置。
  完整偏移表、命令与坑位见 [docs/flashing.md](docs/flashing.md)。

### 7.4 控制通道配置

编译期默认值在 `main/Kconfig.projbuild`（`CONFIG_CONTROL_MQTT_ENDPOINT` 等）；运行期可用 NVS 命名空间 `control_mqtt` 覆盖（endpoint 为空则停用该通道）。

---

## 8. 关键流程

### 8.1 启动

```
main.cc → Board 单例 → Application::Start()
  → 显示/音频初始化 → 读 NVS → OTA 请求（拿主协议地址）
  → ControlMqtt::Start()（第二路通道常连）
  → 进入 Idle，等待唤醒词/按键/远程 listen
```

### 8.2 语音交互

```
唤醒词/按键 → SetDeviceState(Connecting) → OpenAudioChannel
  → Listening：持续采集上传（AEC 开启时 Speaking 中也保持处理）
  → Speaking：播放 TTS；每轮进入 Speaking 上报一次 interaction
  → 服务端 goodbye → CloseAudioChannel → Idle（无本地静音超时兜底）
```

### 8.3 远程图片下发

```
cmd: set_image{url, mode?} → 下载任务（头部预检：≤4MB；解码预检：≤1280px/1MP）
  → JPEG→RGB565 → 主任务建 LvglAllocatedImage
  → mode=preview（默认）：80% 屏宽缩放浮层显示（可拖动、常驻；小图不放大）
  → mode=background：JPEG/PNG（PNG 先解为 ARGB8888），cover 居中裁剪铺满全屏 → 写入 light/dark 主题的
    background_image（LvglTheme 机制）→ SetTheme 重绘 container_ 底层
双击屏幕 → 清预览图（上报 image_cleared；壁纸保留）—— 播报中则改为停止播报，见 8.4
三击屏幕 → 切主题（上报 theme_changed）
{"clear":true} → 同时清预览与壁纸
```

### 8.4 远程语音播报（`notify`）

```
cmd: notify{audio_url, subtitles?, id?} → 仅 Idle 受理 → 进入 Notifying
  → NotifyPlayer: HTTP 增量下载 → Ogg 解复用 → 共享解码队列 → 播放
  → 离开 Notifying 时上报一次 notify_result{id, outcome, duration_ms}
```

`outcome` 判定顺序（命中即止）：`cancelled`（远程 `abort_notify` / 播报中下发 `listen`）
> `user_stopped`（屏幕双击 / BOOT 键 / IMU 手势）> `finished` > `aborted`（唤醒词打断）
> `interrupted`（OTA 等系统动作）。

停止播报的四种途径都走同一套簿记：`ControlMqtt::TrackNotificationState()` 在状态迁移时判定，
因此**任何"先停播报再切状态"的路径都只上报最终状态**——例如唤醒打断是 notifying→connecting
（中间的 idle 被合并），判 `aborted` 而非 `finished`。

---

## 9. 开发约定与注意事项（精要）

完整规则见 [AGENTS.md](AGENTS.md)，高频红线：

1. **线程纪律**：回调可能来自任意任务；修改应用状态必须 `Application::Schedule()`；严禁阻塞主事件循环和音频任务；音频路径避免无界队列与反复大分配。
2. **状态唯一入口**：一律通过 `Application::SetDeviceState()`，不直接改状态变量。
3. **板级隔离**：核心代码只依赖 `Board` 接口，不 include 具体板级 `config.h`；不改既有板型引脚，新硬件新增板型目录。
4. **能力可选**：屏幕/背光/电池/摄像头等均视为可选能力，核心逻辑需兼容无显示板。
5. **生成物勿手改**：`build/`、`managed_components/`、`sdkconfig*`、`main/assets/lang_config.h` 等由工具生成。
6. **cJSON 所有权与 NVS**：注意 JSON 释放责任；NVS 键是持久 API，改动需迁移。
7. **本项目经验**：LVGL 旋转下分辨率/布局用 `LV_HOR_RES/LV_VER_RES`；`xTaskCreateStatic` 可能因对齐问题崩溃，优先 `xTaskCreate`；唤醒词配置需同时满足"使能自定义 + 关闭默认 WakeNet + 使能 MultiNet"三件套。

---

## 10. 文档索引

| 文档 | 内容 |
|---|---|
| [AGENTS.md](AGENTS.md) | 工程规则（必读） |
| [README_zh.md](README_zh.md) | 上游项目总览与快速开始 |
| [docs/control-mqtt.md](docs/control-mqtt.md) | ★ 第二路 MQTT 控制通道完整协议（命令/事件/图片规格/阶段记录） |
| [docs/control-mqtt-notify-feedback.md](docs/control-mqtt-notify-feedback.md) | ★ 播报闭环反馈实施方案 + 10 项真机测试记录与复查结论 |
| [docs/tts-broadcast-design.md](docs/tts-broadcast-design.md) | 远程语音播报服务设计评估（服务端 TTS + 直链下发） |
| [docs/flashing.md](docs/flashing.md) | 3.49 V2 烧录实操（偏移表/命令/注意事项/厂家文档） |
| [docs/virsical-desktop-ai-badge-architecture.md](docs/virsical-desktop-ai-badge-architecture.md) | ★ 威思客桌面 AI 助理 / 电子工牌方案评估与系统架构（含汇报 PPT/PDF） |
| [docs/virsical-release-2.4.2.1-2026-10-26.md](docs/virsical-release-2.4.2.1-2026-10-26.md) | ★ 2.4.2.1 版本策略、2026-10-26 需求与真机测试记录 |
| [docs/websocket.md](docs/websocket.md) / [docs/mqtt-udp.md](docs/mqtt-udp.md) | 主协议两种传输 |
| [docs/mcp-protocol.md](docs/mcp-protocol.md) / [docs/mcp-usage.md](docs/mcp-usage.md) | MCP 协议与用法 |
| [docs/custom-board.md](docs/custom-board.md) | 新板接入指南 |
| [docs/esp-idf-6-migration.md](docs/esp-idf-6-migration.md) | IDF 6 迁移说明 |
| [docs/code_style.md](docs/code_style.md) | 代码风格 |
| [main/audio/README.md](main/audio/README.md) | 音频设计 |
| [scripts/control_mqtt_monitor.py](scripts/control_mqtt_monitor.py) | 控制通道监控工具 |

---

## 11. 已知限制与待办

- OTA 升级执行已禁用（仅保留版本有效性标记），远程升级需另行恢复；
- `set_image` 的 `mode=background` 支持 JPEG/PNG；PNG 需在显示层解为 ARGB8888 再转 RGB565，1MP 级 PNG 的内存峰值较高，解码或分配失败会回 `success=false`；2026-10-26 在 3.49 V2 实测：PNG 640×172/800×450 成功，PNG 1024×576/1200×800 失败；JPEG 640×172/1200×800/1280×720 成功。测试文件仅 2–29KB，失败来自解码像素内存而非文件大小；大图背景优先用 JPEG 或 PNG ≤800×450；
- `interaction` 事件以"进入 Speaking"计数，服务端拆分 TTS 时可能多计；
- 聆听→待机由服务端 goodbye 驱动，设备无本地静音超时兜底；
- host 测试存在 1 项失败：板级 `config.json` 的 `CONFIG_SR_WN_...=n` 触发上游「唤醒词不得写在板级配置」断言，跑 unittest/CI 会红，本地 `build.py` 不受影响；
- IMU 运动检测灵敏度偏高（0.1g 阈值），静态放置偶发误触发；屏幕点击与按键按下会打时间戳、其后 700ms 忽略运动（抑制点击机身抖动），但仍无法区分"用户拿起来晃动"和"桌面被碰了一下"；调试时可用 `{"type":"set_imu","enable":false}` 关闭（持久化到 NVS `imu/enabled`）；
- `notify` 的音频 URL 必须是不跳转的直链，`http://` 与 `https://` 均可；跨网段/高 RTT 链路出现过首包超时（5s 超时），生产环境音频应放同网段或公网 CDN；
- 控制通道事件为 QoS 0，设备离线期间的事件不补发；
- `PROJECT_VER` 为 **`2.4.2.1`**（上游基础版本 `2.4.2` + fork 修订号 `.1`，数字后缀保证 OTA 版本解析兼容）；2026-10-25 只合入上游 v2.5.2 的部分局部修复（阶段十四），不是完整升级，MCP 错误码/`max_length`、C++ 异常关闭、`NetworkResult`、epd 公共类等未合入；
- 4B 板级目录已有，但 fork 的 480×480 工牌 UI、手势、IMU/RTC UI 尚未对齐（一期不做电量 UI）；当前构建/验证仍以 3.49 为主；4B 的 `config.json` 已同步“你好，小威”唤醒词配置，尚未做 4B 全量构建/真机验证。
- 小智云支持私有化部署，当前方案仍按 SaaS 试点；私有化/与企业服务端合并属于二期/三期，需评估 GPU、运维、许可和数据合规。

---

## 12. 变更记录（2026-10-25）

- 壁纸支持 PNG：`CreateCoverBackground()` 在显示层用 LVGL lodepng 解为 ARGB8888，再 cover 裁剪并转 RGB565（透明像素叠黑底）；`SetBackgroundImage()` 改为返回 `bool`，失败不再假成功。
- 手势拆分：双击清除双击时的那张预览图（500ms 确认窗口），三击切换主题；三击取消待执行的双击清图，不会顺带清图。
- IMU 增加运行时开关：`set_imu` 命令，NVS `imu/enabled` 持久化；`Board` 增加 `HasImu/SetImuEnabled/IsImuEnabled` 可选接口。
- `query_status` 增加 `theme/volume/brightness/preview_active/wallpaper_active/imu_enabled/wake_word_running` 诊断字段。
- 预览小图不再放大到屏宽 80%，最高保持原尺寸。
- 路线 A 上游补丁：MQTT `SendAudio` 解锁、WebSocket `transport` 类型校验、主题短颜色保护、cJSON 自定义消息释放、OGG 末尾零长度 lace、OTA 版本非抛异常解析；详见 [docs/control-mqtt.md](docs/control-mqtt.md) 阶段十三/十四。
- 硬件目标切换为 **ESP32-S3-Touch-LCD-4B**（4" 480×480 方形屏），当前继续用 3.49 V2 做固件/音频/控制基线测试；4B 的工牌 UI、手势、IMU/RTC UI 仍需对齐（一期不做电量 UI）。
- 唤醒词统一为 **“你好，小威”**：3.49 测试板显示文案更新，4B `config.json` 补齐自定义唤醒词/MultiNet 配置（拼音 `ni hao xiao wei`）。
- 小智云部署路径：一期 SaaS 试点，二期私有化 PoC，三期评估与企业服务端合并；合并原则见方案文档。
- 验证：3.49 测试板应用固件重新编译/链接，`xiaozhi.bin` 为 `0x2eef20`，分区余量 25.5%；合并固件已更新；改动文件 `-Wall -Wextra` 无 warning；host 测试 66/67，唯一失败仍是板级唤醒词配置断言。
- 2026-10-26 真机手势验证：3.49 V2 双击清图、三击切主题均通过；`query_status` 确认 `preview_active=false`、`wallpaper_active=false`、`theme=light`。

---

## 13. 2026-10-26：版本 2.4.2.1 与真机验证

- `PROJECT_VER` 调整为 **`2.4.2.1`**：上游基础版本 `2.4.2` + fork 修订号 `.1`，数字后缀保持 OTA 版本解析兼容。
- 3.49 V2 最新固件重新构建并刷入 OTA_0/OTA_1，保留 NVS/WiFi 配置；启动日志确认 `App version: 2.4.2.1`。
- 真机验证通过：双击清图、三击切主题、IMU 开关、主题切换、状态提示、JPEG/PNG 临时预览、JPEG 背景、PNG 背景。
- 图片实测：JPEG 1280×720 背景成功；PNG 800×450 成功，PNG 1024×576/1200×800 失败（文件很小，判断为解码像素内存问题）。
- 详细需求、测试矩阵和已知问题见 [docs/virsical-release-2.4.2.1-2026-10-26.md](docs/virsical-release-2.4.2.1-2026-10-26.md)。

---

## 14. 2026-10-26（下午）：播报闭环反馈与本地停止手势

面向"服务平台下发语音播报"场景，补齐送达确认与本地停止能力。改动集中在 `control_mqtt.{h,cc}`、
`application.{h,cc}` 与 3.49-v2 板级，未触碰 `notify/`、`protocols/` 与主协议。

**设备侧增量**

| 增量 | 说明 |
|---|---|
| `notify` 回执语义修复 + 可选 `id` | 原先无条件回 `success=true`；现复查是否真的进入 `notifying`，`id` 回传用于对账 |
| 新增上行 `notify_result` | `outcome`（`finished`/`cancelled`/`user_stopped`/`aborted`/`interrupted`）+ `duration_ms`，服务端不必再靠时长猜结果 |
| 新增下行 `abort_notify` | 远程取消播报，清空解码与播放队列立即静音 |
| `query_status` 增加字段 | `notify_active`、`notify_id` |
| 本地手势停止播报 | 播报中**屏幕双击**停止（不清图、不连带切主题）；BOOT 键与 IMU 姿态变化同样可停止，统一上报 `outcome=user_stopped` |
| IMU 改为触发聆听 | 由 `ToggleChatState()` 改为 `StartListening(GetDefaultListeningMode())`；触发状态由「仅 Idle」放宽到「Idle + Notifying」 |
| 点击抖动抑制 | `ImuMonitor::NotifyUserInteraction()`：屏幕点击与 BOOT/PWR 按键打时间戳，其后 700ms 忽略运动；窗后第一拍也丢弃 |
| `Application` 新增接口 | `StopNotificationByUser()`（主线程专用）、`StartListening(ListeningMode)` 重载 |

**两个易踩的设计约束**（详见 [docs/control-mqtt-notify-feedback.md](docs/control-mqtt-notify-feedback.md)）：

1. 停止播报必须**先标记再改状态**：直接让状态转到 idle 会被判成 `finished`（37s 音频播 3s 却报"播完"）。
2. 远程标记**优先于**本地标记：播报中下发 `listen` 会同时命中两条路径，若顺序颠倒会被误报成机身手势。

**真机验证**：10 项用例全部通过（正常播报 / 远程取消 / 空闲取消 / 唤醒打断 / 双击停播报 / IMU 触发聆听 /
喇叭不误触发 / 双击清图 / 点击抖动抑制 / 失败场景）。固件 `xiaozhi.bin` = `0x2f0040`，分区余 25%。
唤醒词为「**你好，小威**」（非「你好小智」），首次测唤醒打断失败就是因为说错了唤醒词。
