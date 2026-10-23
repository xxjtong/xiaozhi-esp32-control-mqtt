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
| **硬件适配** | 适配 Waveshare ESP32-S3-Touch-LCD-3.49 **V2** 开发板（3.49" 条形触摸屏 + IMU + 电池） | [main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/](main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/) |
| **第二路 MQTT 控制平面** | 在官方语音协议之外，新增一条独立的 MQTT 通道，实现远程控制（发文字/图片/主题/音量等）与本地用户行为事件上报 | [main/control_mqtt.cc](main/control_mqtt.cc) · [docs/control-mqtt.md](docs/control-mqtt.md) |

### 1.3 快速事实卡

| 项 | 值 |
|---|---|
| 语言/框架 | C/C++，ESP-IDF v6.0.2 |
| 支持芯片 | ESP32 / S2 / S3 / C2 / C3 / C5 / C6 / P4（见 `sdkconfig.defaults.*`） |
| 板级支持 | `main/boards/` 下 46 个板级目录，本 fork 实际使用 `waveshare/esp32-s3-touch-lcd-3.49-v2` |
| 构建入口 | `python3 scripts/build.py <board> --name <variant>` |
| 主协议服务端 | xiaozhi.me（OTA 分配，MQTT+UDP 或 WebSocket 二选一） |
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
│   │   └── waveshare/esp32-s3-touch-lcd-3.49-v2/  # ★ 本 fork 目标板
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
  - `ReportUserEvent(event, value)`（L1176 起）：把本地用户行为事件经 `Schedule()` 后由 ControlMqtt 发布；
  - 状态机监听器（L93 起）：每次进入 `Speaking` 记一次 `interaction` 事件（即一轮人机交互）；
  - 唤醒词触发时上报 `wake_word` 事件。

### 4.2 音频子系统（[main/audio/](main/audio/)，另有详细 [main/audio/README.md](main/audio/README.md)）

- `AudioService`：统一调度采集、播放、唤醒词、VAD、Opus 编解码。
- 链路：麦克风 → AFE（AEC/NS/VAD）→ 16k PCM → Opus（60ms VBR + DTX）→ 主协议上传；下行 TTS 音流反向播放。
- 唤醒词：WakeNet 内置模型或自定义唤醒词（本 fork 配置了自定义唤醒词，见 5.1）。
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
  - 预览图容器 `preview_viewport_`：全屏透明滚动容器，图片按**屏宽 80%** 等比缩放居中，支持上下拖动，双击清图（[main/display/lcd_display.cc](main/display/lcd_display.cc)）。

### 4.5 板级抽象（[main/boards/common/board.h](main/boards/common/board.h)）

- `Board` 单例接口：`GetNetwork()/GetDisplay()/GetAudioCodec()/GetUuid()/GetBoardJson()` 等。
- 本 fork 新增可选接口：`GetIpAddress() / GetWifiSsid() / GetWifiRssi()`（默认空实现），`WifiBoard` 从 WifiManager 取值，`DualNetworkBoard` 委托当前子板。
- 每个板级目录含：`config.h`（引脚/宏）、板级 `.cc`（唯一 `DECLARE_BOARD(...)` 工厂）、可选 `custom_lcd_display.cc` 等定制类。

### 4.6 MCP 服务端（[main/mcp_server.cc](main/mcp_server.cc)）

设备作为 MCP Server，向云端 LLM 暴露工具（音量、亮度、主题、播报等）；云端经主协议下发 `mcp` JSON-RPC 消息调用。第二路控制通道的 `mcp` 命令可复用该分发逻辑（见 5.3）。

---

## 5. 本 fork 定制实现详解

### 5.1 目标板：Waveshare ESP32-S3-Touch-LCD-3.49 V2

| 硬件 | 说明 |
|---|---|
| 屏幕 | AXS15231B QSPI，物理 172×640，`DISPLAY_ROTATION_90` + `lv_display_set_rotation(270)` → 可用区域 **640×172** 横条屏 |
| 音频 | ES8311（DAC）+ ES7210（ADC），本板开启 `CONFIG_USE_DEVICE_AEC=y`（回声消除，Realtime 连续聆听模式） |
| IO 扩展 | TCA9554（背光/功放使能等） |
| IMU | 运动检测：状态改变视为用户行为，触发 `ToggleChatState()` 并上报 `motion_detected` 事件 |
| 电池 | ADC1_CH3 (GPIO4)，200kΩ/100kΩ 3:1 分压采样 |
| 触摸 | 双击屏幕任意区域切换明暗主题（浅色/深色），有常驻图时先清图 |

代码：[main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/](main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/)（含 `custom_lcd_display.cc`、`imu_monitor.h`）。

> 关键经验：LVGL 布局必须用 `LV_HOR_RES/LV_VER_RES`（640×172）而非面板原始 `width_/height_`（172×640），否则旋转后 UI 错位。

### 5.2 控制通道生命周期（[main/control_mqtt.cc](main/control_mqtt.cc)）

- 配置：NVS 命名空间 `control_mqtt`（键：`endpoint/client_id/username/password/subscribe_topic/reply_topic`），缺省回落 Kconfig 默认值；`endpoint` 为空则不启动。
- 主题约定：订阅 `xiaozhi/<device_id>/cmd`，发布 `xiaozhi/<device_id>/reply`（`device_id` 为板级 UUID）。
- 连接：独立 FreeRTOS 任务执行 `Mqtt::Connect()`（超时自控）；失败**不销毁客户端**，交由 esp-mqtt 自动重连（默认开启，约 10s 间隔），重连成功后重新订阅并广播 `hello`。
- JSON 处理全部在主任务执行（回调里 `Schedule()` 投递），遵守单线程纪律。
- 线程安全：`lifecycle_mutex_` 保护连接任务与析构竞争；`image_fetch_running_` 原子量保证图片下载单飞。

### 5.3 控制命令接口（下行：`xiaozhi/<id>/cmd`）

所有命令为 JSON，`type` 字段区分；执行结果经 reply 主题回执 `{"type":"reply","command":"<type>","success":true|false,...}`。

| type | 功能 | 主要参数 | 状态约束 |
|---|---|---|---|
| `listen` | 远程开/关聆听 | `state: "start"|"stop"` | start 仅限 Idle/Notifying；stop 任意时刻 |
| `abort` | 中断当前会话/说话 | `reason`（可选） | 任意时刻 |
| `mcp` | 调用设备端 MCP 工具 | MCP JSON-RPC 消息体 | 仅 Idle（避免与主会话冲突） |
| `system` | 系统命令 | `command: "reboot"` 等 | — |
| `set_image` | 下发/清除图片 | `url`（HTTP/JPEG·PNG）或 `clear:true` | 图片常驻显示 |
| `set_theme` | 切换明暗主题 | `name: "dark"|"light"` | — |
| `set_volume` | 设置音量 | `volume: 0-100` | — |
| `set_brightness` | 设置背光 | `brightness: 0-100` | — |
| `toggle_wake_word` | 开关唤醒词 | — | — |
| `notify` | 播放提示音/通知 | — | 仅 Idle |
| `set_status` | 状态栏文字（可滚动） | `text`，可选 `color`（色名或 `#RRGGBB`） | — |
| `toggle_power_save` | 切换省电模式 | — | — |
| `query_status` | 查询设备状态 | — | extra 返回 `ip/ssid/rssi`（有值才带） |

**图片链路实现**（`HandleSetImage` → `ImageFetchTaskEntry`）：
1. 下载任务（独立任务，栈 12KB，原子量单飞）；
2. HTTP 头部预检 `Content-Length` > 4MB 直接拒绝（不下载）；
3. JPEG/PNG 头部解析宽高（扫 SOF / IHDR），超过 1280×1280 或 100 万像素拒绝（不分配解码内存）；
4. JPEG 经 `jpeg_to_image` 解为 RGB565，主任务构造 `LvglAllocatedImage`；
5. 显示：按 `LV_HOR_RES*80%` 等比缩放、垂直居中、可上下拖动；`CancelPreviewTimeout()` 使图片常驻（不自动消失）。

### 5.4 上行消息（`xiaozhi/<id>/reply`）

| type | 时机 | 关键字段 |
|---|---|---|
| `hello` | 连接/重连成功后 | `device_id/status/version/ip` |
| `reply` | 每条命令的执行回执 | `command/success/message` |
| `goodbye` | 停用/析构前 | — |
| `event` | 本地用户行为 | `device_id/event/value/state/uptime_ms/seq` |

**用户行为事件表**（QoS 0 尽力而为，`seq` 单调递增，只含元数据不含音频/文本内容）：

| event | 触发点 | 采集实现 |
|---|---|---|
| `wake_word` | 唤醒词命中 | `HandleWakeWordDetectedEvent` 入口 |
| `interaction` | 一轮人机交互（进入 Speaking 计一次） | 状态机监听器 `old!=Speaking && new==Speaking` |
| `button_click` | BOOT 按键单击 | 板级 `boot_button_.OnClick` |
| `motion_detected` | IMU 检测到姿态变化 | `imu_monitor.h` |
| `image_cleared` | 双击清除常驻图片 | `custom_lcd_display.cc` |
| `theme_changed` | 双击切换主题 | `custom_lcd_display.cc`，value=新主题名 |

### 5.5 网络信息上报

- `Board` 层新增 `GetIpAddress/GetWifiSsid/GetWifiRssi` 可选接口（[main/boards/common/board.h](main/boards/common/board.h)），`WifiBoard` 实现之；
- `hello` 消息携带 `ip`；`query_status` 的 extra 携带 `ip/ssid/rssi`。

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
python3 scripts/build.py waveshare/esp32-s3-touch-lcd-3.49-v2 \
        --name esp32-s3-touch-lcd-3.49-v2                                # 构建本 fork 目标板
python3 -m unittest discover -s scripts/tests -v                         # host 侧测试
```

> 板型选择是耦合链路：`config.json` → `scripts/build.py` → `main/Kconfig.projbuild` → `main/CMakeLists.txt` → 板级源码与 `config.h`。新增板型必须打通每一环，且 `DECLARE_BOARD(...)` 全工程唯一（板型标识影响 OTA 兼容性）。

### 7.3 烧录（本 fork 实测路径）

- 串口：`/dev/cu.usbmodem1101`（macOS，注意用 `cu.` 而非 `tty.`）；
- 可直接 `idf.py -p /dev/cu.usbmodem1101 flash monitor`，或在 python 环境不一致时改用 esptool 命令行按 `@flash_args` 烧录。

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
cmd: set_image{url} → 下载任务（头部预检：≤4MB；解码预检：≤1280px/1MP）
  → JPEG→RGB565 → 主任务建 LvglAllocatedImage → 80% 屏宽缩放显示（可拖动、常驻）
双击屏幕 → 清图（上报 image_cleared）→ 切主题（上报 theme_changed）
```

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
- `interaction` 事件以"进入 Speaking"计数，服务端拆分 TTS 时可能多计；
- 聆听→待机由服务端 goodbye 驱动，设备无本地静音超时兜底；
- host 测试存在 1 项既存失败（板级 `config.json` 唤醒词断言，与本 fork 功能无关）；
- IMU 运动检测灵敏度偏高（0.1g 阈值），静态放置偶发误触发；
- 控制通道事件为 QoS 0，设备离线期间的事件不补发。
