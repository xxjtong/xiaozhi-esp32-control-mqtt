# 固件定制 API 与功能总览

> 适用代码：`xiaozhi-esp32` fork，`PROJECT_VER = 2.5.2.1`（上游 2.5.2 基线）
> 最后核对：2026-11-06
>
> 本文是设备端定制能力的"总账"，面向固件维护者与对接方。控制通道的逐字段报文、错误码与
> 完整示例见 [`control-mqtt-api.md`](./control-mqtt-api.md)；设计取舍与改动历史见
> [`control-mqtt.md`](./control-mqtt.md)；MCP 协议见 [`mcp-protocol.md`](./mcp-protocol.md)。

## 0. 口径与分层

本仓库相对上游的定制集中在两条主线：

1. **第二路 MQTT 控制平面**：独立于小智主协议，用于远程控制、状态查询与用户行为上报。
2. **板级交互与能力扩展**：4B / 3.49 V2 的屏幕手势、按键、IMU、RTC、logo、壁纸、
   远程播报反馈，以及为支撑它们新增的 `Board` / `Display` / `Application` / `AudioService`
   可选接口。

按调用方分为四层：

| 层 | 调用方 | 形式 | 定制属性 | 代码入口 |
|---|---|---|---|---|
| 控制通道 API | 业务服务端 | MQTT + JSON | 本 fork 新增 | [`control_mqtt.cc`](../main/control_mqtt.cc) |
| MCP API | 云端 LLM / 控制通道 | JSON-RPC | 上游标准，本 fork 未改 | [`mcp_server.cc`](../main/mcp_server.cc) |
| C++ 扩展 API | 固件内部、板级实现 | 虚函数/成员函数 | 本 fork 新增 | [`board.h`](../main/boards/common/board.h)、[`display.h`](../main/display/display.h)、[`application.h`](../main/application.h) |
| 板级功能 | 最终用户 | 手势/按键/硬件 | 本 fork 新增 | 各板级目录 |

> 判定原则：只有上游没有、或语义被本 fork 改动的接口才列入"定制"；控制通道的完整报文
> 契约以 [`control-mqtt-api.md`](./control-mqtt-api.md) 为准，本文不重复所有 JSON 示例。

---

## 1. 控制通道 API

### 1.1 连接与配置

| 项 | 值 / 说明 |
|---|---|
| 用途 | 纯控制平面：命令、回执、状态、事件、图片、播报；不传音频、不参与小智会话 |
| 客户端 | 独立 MQTT 客户端（`connect_id=1`；官方通道为 `0`） |
| 默认 broker | `mqtt.virsical.cn:21883`（明文 TCP；`8883` 走 TLS） |
| 默认凭据 | `admin` / `public`（Kconfig 可改，生产应由 NVS 注入） |
| `device_id` | `Board::GetInstance().GetUuid()` |
| `client_id` | 默认 `xiaozhi-<UUID 后 8 位>` |
| 下行 topic | `xiaozhi/<device_id>/cmd`，NVS 键 `subscribe_topic` |
| 上行 topic | `xiaozhi/<device_id>/reply`，NVS 键 `reply_topic` |
| 配置来源 | NVS 命名空间 `control_mqtt` 优先，Kconfig 兜底；`endpoint` 为空则通道不启动 |
| QoS / 编码 | QoS 0（尽力而为）；UTF-8、无空格 JSON |
| 生命周期 | 开机常连；断线由 esp-mqtt 自动重连，重连后重新订阅并补发 `hello` |

NVS 键：`endpoint`、`client_id`、`username`、`password`、`subscribe_topic`、`reply_topic`。

### 1.2 下行指令（15 条）

发布到 `cmd` topic；每条指令必须有 `type`，处理完有且仅有一次 `reply`。

| # | `type` | 必填 / 可选 | 前置状态 | 功能 |
|---|---|---|---|---|
| 1 | `listen` | `state: start\|stop` | `start`: Idle/Notifying；`stop`: 任意 | 远程开始/停止聆听；`start` 走 `ToggleChatState()` |
| 2 | `abort` | 可选 `reason: wake_word_detected` | Connecting/Listening/Speaking | 打断当前回合 |
| 3 | `mcp` | `payload`（JSON 对象） | Idle | 透传到设备 MCP 管道 |
| 4 | `system` | `command: reboot` | 任意 | 先回执再重启；目前只实现 `reboot` |
| 5 | `set_image` | `url` 或 `clear:true`；可选 `mode` | 需 LVGL 显示 | 远程图片；`preview` 浮层 / `background` 壁纸 |
| 6 | `set_theme` | `theme: light\|dark` | 需带主题显示 | 切换明暗主题 |
| 7 | `set_volume` | `level: 0–100` | 需音频 Codec | 设置音量；钳位并持久化 |
| 8 | `set_brightness` | `level: 0–100` | 需 Backlight | 设置背光；钳位并持久化 |
| 9 | `toggle_wake_word` | 可选 `enable: bool` | Idle | 开关唤醒词检测 |
| 10 | `notify` | `audio_url`；可选 `subtitles[]`、`id` | Idle | 远程语音播报 |
| 11 | `abort_notify` | — | Notifying | 取消播报，清空解码/播放队列 |
| 12 | `set_status` | `text`；可选 `color` | 需显示 | 状态栏文字（可滚动）与颜色 |
| 13 | `toggle_power_save` | 可选 `enable: bool` | Idle | 低功耗 / 性能模式 |
| 14 | `set_imu` | 可选 `enable: bool` | 任意 | 开关 IMU 运动检测；持久化到 NVS |
| 15 | `query_status` | — | 任意 | 查询设备全量状态 |

状态不满足时回 `success=false`，不会静默吞掉；未知 `type` 也回执并把原值放进 `command_type`。

### 1.3 上行消息（6 类）

全部发布到 `reply` topic。

| `type` | 触发时机 | 关键字段 |
|---|---|---|
| `hello` | 连接成功（含每次重连） | `device_id`、`status`、`version`、`ip`（有值时） |
| `reply` | 每条下行指令处理完 | `command_type`、`success`、`device_state`、`id`（有值时）、`extra`（有值时）；**无 `device_id`** |
| `goodbye` | 主动断开前 | `device_id` |
| `session_update` | 设备状态每次变化（控制通道在线时） | `device_id`、`status` |
| `notify_result` | 播报离开 `notifying` 时（一次） | `device_id`、`id`（有值时）、`outcome`、`duration_ms` |
| `event` | 本地用户行为 | `device_id`、`event`、`value`（有值时）、`state`、`uptime_ms`、`seq` |

`state` / `status` 取值：`unknown`、`starting`、`wifi_configuring`、`idle`、`connecting`、
`listening`、`speaking`、`notifying`、`upgrading`、`activating`、`audio_testing`、`fatal_error`。

### 1.4 本地事件（7 种）

| `event` | `value` | 触发条件 |
|---|---|---|
| `wake_word` | — | 唤醒词命中（含打断说话/聆听/播报） |
| `interaction` | — | 每次进入 `speaking`，即一轮"提问→回答" |
| `button_click` | `boot` | BOOT 键短按 |
| `motion_detected` | — | IMU 姿态变化（Idle/Notifying，5 s 冷却，点击后 700 ms 抑制） |
| `notify_stopped` | `double_click` | 播报中屏幕双击停止 |
| `image_cleared` | `double_click` | 双击清除常驻预览图（播报中双击改为停播报） |
| `theme_changed` | `light` / `dark` | 三击切换主题 |

事件为 QoS 0，离线期间直接丢弃不补发；`seq` 单调递增，可用于判重/排序。

### 1.5 播报结果（5 种 outcome）

判定顺序自上而下，命中即止：

| `outcome` | 含义 | 触发来源 |
|---|---|---|
| `cancelled` | 远程取消 | `abort_notify`；播报中下发 `listen` |
| `user_stopped` | 本地用户停止 | 屏幕双击、BOOT 键、IMU 手势 |
| `aborted` | 被接管 | 唤醒词打断（转 connecting/listening/speaking） |
| `interrupted` | 被系统动作打断 | 转 activating / upgrading（如 OTA） |
| `finished` | 音频播完**或播放失败** | 播放完成回调 |

`finished` 不区分成功与失败，服务端需结合 `duration_ms` 判断；远程标记优先于本地标记。

### 1.6 图片与播报约束

| 项 | 限制 |
|---|---|
| 图片格式 | JPEG（baseline）/ PNG |
| 文件体积 | ≤ 4 MB（看 `Content-Length`，超限不下载） |
| 分辨率 | ≤ 1280×1280 且 ≤ 100 万像素 |
| 并发 | 同一时刻只允许一个下载任务 |
| `preview` | 按屏宽 80% 等比缩放（小图不放大），垂直居中、可拖动、常驻，双击或 `clear:true` 清除 |
| `background` | cover 居中裁剪铺满全屏，跨主题保留，仅 `clear:true` 清除；重启恢复纯色 |
| 播报音频 | 不跳转直链，Opus 编码 OGG；HTTP 超时 20 s（覆盖 TLS 握手 + 首包） |

> 4B 当前目标上限为 480×480：JPEG 预览/壁纸可用；PNG 预览可用，PNG 壁纸仍受
> LVGL ARGB 解码缓冲限制。4B 板级关闭了 `CONFIG_SPIRAM_RODATA`，为图像缓冲腾出
> PSRAM；3.49 V2 等其他板型不受影响。
| 播报回执 | `success=true` 仅表示已接受并开始播放，最终以 `notify_result` 为准 |

### 1.7 错误与诊断

`success=false` 时，部分指令在 `extra.error` 给出原因（完整清单见
[`control-mqtt-api.md`](./control-mqtt-api.md) §5）：

- `set_image`：`no display`、`failed to create image task`、`another image download is in progress`、
  `image too large (max 4 MB)`、`image too large (max 1280x1280, 1 MP)`、
  `jpeg decode failed`、`unsupported image data`、`out of memory`、
  `failed to apply background image`、`http open failed`、`http status is not 200` 等。
- `set_theme`：`missing theme name`、`display does not support themes`、`unknown theme`。
- `set_status`：`unknown color`。
- `set_imu`：`IMU not available on this board`。

其余指令失败通常只回 `success=false`，由状态约束或硬件能力缺失导致。

---

## 2. MCP API

设备作为 MCP Server，向云端 LLM 暴露工具。调用路径有两条：

1. 主协议（小智云）下发的 `mcp` JSON-RPC 消息；
2. 控制通道下行 `{"type":"mcp","payload":{...}}`（仅 Idle）。

> [`mcp_server.cc`](../main/mcp_server.cc) 与上游 2.5.2 一致，本 fork **没有新增或修改 MCP 工具**；
> 下面是当前固件实际注册的工具，供对接时确认能力边界。

### 2.1 通用工具（AI 可见，5 个）

| 工具 | 功能 | 注册条件 |
|---|---|---|
| `self.get_device_status` | 实时设备状态（音频、屏幕、电池、网络等） | 始终 |
| `self.audio_speaker.set_volume` | 设置音量 0–100 | 始终 |
| `self.screen.set_brightness` | 设置亮度 0–100 | 有 Backlight |
| `self.screen.set_theme` | 切换 `light`/`dark` | 有 LVGL 且显示支持主题 |
| `self.camera.take_photo` | 拍照并让云端解释图片 | 有 Camera（且 LVGL 构建） |

### 2.2 仅用户可见工具（`audience: user`，7 个）

| 工具 | 功能 | 注册条件 |
|---|---|---|
| `self.get_system_info` | 系统信息 | 始终 |
| `self.reboot` | 重启设备 | 始终 |
| `self.upgrade_firmware` | 从 URL 手动升级 | 始终 |
| `self.screen.get_info` | 屏幕宽高、是否单色 | 显示支持 GUI 操作 |
| `self.screen.snapshot` | 截图并 multipart 上传 | `CONFIG_LV_USE_SNAPSHOT` |
| `self.screen.preview_image` | 下载并预览图片 | `CONFIG_LV_USE_SNAPSHOT` |
| `self.assets.set_download_url` | 设置 assets 下载地址 | 始终 |

---

## 3. C++ 扩展 API

以下接口全部带默认实现，未实现该能力的板型无需改动；调用方必须先判空或看返回值。

### 3.1 `Board`：网络与 IMU 可选能力

[`board.h`](../main/boards/common/board.h)

```cpp
virtual std::string GetIpAddress() { return ""; }
virtual std::string GetWifiSsid() { return ""; }
virtual int GetWifiRssi() { return 0; }

virtual bool HasImu() { return false; }
virtual bool SetImuEnabled(bool enabled);   // 默认返回 false
virtual bool IsImuEnabled() { return false; }
```

| 接口 | 语义 | 实现 |
|---|---|---|
| `GetIpAddress/GetWifiSsid/GetWifiRssi` | 板级网络信息；无数据时返回空串/0 | `WifiBoard` 实现；`DualNetworkBoard` 委托当前板 |
| `HasImu` | 是否存在 IMU | 4B / 3.49 V2 返回 `imu_available_` |
| `SetImuEnabled` | 运行时开关运动检测 | 板级实现并持久化到 NVS `imu/enabled` |
| `IsImuEnabled` | 查询当前开关 | 板级实现 |

### 3.2 `Display`：状态颜色、预览图与壁纸

[`display.h`](../main/display/display.h)

```cpp
virtual void SetStatusColor(uint32_t rgb);
virtual void CancelPreviewTimeout() {}
virtual bool SetBackgroundImage(std::unique_ptr<LvglImage> image);
virtual void ClearBackgroundImage() {}
virtual bool HasPreviewImage() { return false; }
virtual bool HasBackgroundImage() { return false; }
```

| 接口 | 语义 |
|---|---|
| `SetStatusColor` | 覆盖状态栏文字颜色（`0xRRGGBB`）；下一次 `SetStatus()` 恢复主题色 |
| `CancelPreviewTimeout` | 停掉预览图自动隐藏，使其常驻 |
| `SetBackgroundImage` | 设为桌面壁纸（cover 裁剪）；失败返回 `false` |
| `ClearBackgroundImage` | 清除壁纸 |
| `HasPreviewImage/HasBackgroundImage` | 供 `query_status` 诊断 |

实现链：`LvglDisplay` → `LcdDisplay` → 板级 `CustomLcdDisplay`。OLED 等无壁纸能力的显示保持默认实现。

### 3.3 `Backlight`：背光回读修复

[`backlight.h`](../main/boards/common/backlight.h)

```cpp
virtual bool ReapplyBrightness();
```

`SetBrightness()` 在软件记录值未变化时会直接返回，无法修复"外设复位导致 PWM 丢失、但软件状态仍正确"的背光全灭。`ReapplyBrightness()` 重新写硬件，返回 `true` 表示确实修复了不一致。3.49 V2 的守护任务每秒调用一次。

### 3.4 `Application`：事件上报、聆听与播报

[`application.h`](../main/application.h)

```cpp
void ReportUserEvent(const std::string& event, const std::string& value = "");

void StartListening(ListeningMode mode);
ListeningMode GetDefaultListeningMode() const;

void StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles);
void StopNotificationByUser();
```

| 接口 | 语义 / 约束 |
|---|---|
| `ReportUserEvent` | 上报本地用户事件到控制通道；内部 `Schedule()` 到主任务，捕获调用时状态 |
| `StartListening(mode)` | 线程安全的聆听入口；`pending_listening_mode_` 决定本次聆听模式 |
| `GetDefaultListeningMode` | 返回当前默认聆听模式，供手势/IMU 复用原有自动停止行为 |
| `StartNotification` | 由 private 改为 public；进入 `notifying`，接受后同步返回 |
| `StopNotificationByUser` | 本地手势专用：先标记 `user_stopped` 再停播报，避免被误报为 `finished`；主任务调用 |

相关辅助：[`device_state_str.h`](../main/device_state_str.h) 的
`DeviceStateToString(DeviceState)` 提供控制通道使用的稳定状态字符串。

### 3.5 `AudioService` / `AudioEngine`：按状态门控 AEC

[`audio_service.h`](../main/audio/audio_service.h) / [`audio_engine.h`](../main/audio/audio_engine.h)

```cpp
void AudioService::SetAecRequired(bool required);
virtual void AudioEngine::SetAecRequired(bool required);
```

- 与 `EnableDeviceAec()` 的总开关是 **AND** 关系。
- `Speaking / Notifying` 为 `true`；`Listening / Idle / Connecting / Activating` 等为 `false`。
  Listening 阶段本地没有播放，AEC 关闭以避免 core 0 过载。
- 只影响 AEC，唤醒词检测（MultiNet/AFE）在空闲态仍持续运行。
- 调用于状态机回调（[`application.cc`](../main/application.cc)），避免空闲态占用 AEC 资源。

### 3.6 `ControlMqtt`：控制通道公共接口

[`control_mqtt.h`](../main/control_mqtt.h)

```cpp
bool Start();
void Stop();

void BroadcastState(const std::string& state_name);
void BroadcastEvent(const std::string& event, const std::string& value,
                    const std::string& state);

bool IsConfigured() const;
bool IsConnected() const;

void MarkNotificationUserStopped();
```

| 接口 | 用途 |
|---|---|
| `Start/Stop` | 启动/停止第二路连接；`Start()` 在无配置时静默跳过 |
| `BroadcastState` | 状态变化时更新播报簿记并发布 `session_update` |
| `BroadcastEvent` | 发布本地事件（QoS 0，离线丢弃） |
| `IsConfigured/IsConnected` | 配置与链路状态查询 |
| `MarkNotificationUserStopped` | 标记本地用户停止，供 `notify_result` 归因 |

---

## 4. 功能矩阵

### 4.1 屏幕手势

| 操作 | 4B（480×480） | 3.49 V2（640×172） |
|---|---|---|
| 单击 | 无动作（仅打 IMU 抑制时间戳） | 同左 |
| 双击 · 播报中 | 停止播报，上报 `notify_stopped`，不清图、不切主题 | 同左 |
| 双击 · 有预览图 | 清除双击时看到的那张图，上报 `image_cleared` | 同左 |
| 双击 · 空闲无图 | **进入聆听**（`StartListening(GetDefaultListeningMode())`） | **无动作** |
| 双击 · 聆听中 | **返回空闲**（`StopListening()`） | 未实现 |
| 双击 · 说话中无图 | **先打断播放，再进入聆听** | 未实现 |
| 三击 | 切换 `light`/`dark`，上报 `theme_changed` | 同左 |

双击有 500 ms 确认窗口；第三击会取消待执行的双击动作，只切主题。实现：
[`4B custom_lcd_display.cc`](../main/boards/waveshare/esp32-s3-touch-lcd-4b/custom_lcd_display.cc)、
[`3.49 custom_lcd_display.cc`](../main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/custom_lcd_display.cc)。

### 4.2 BOOT 按键

| 操作 | 4B | 3.49 V2 |
|---|---|---|
| 单击 · 启动阶段 | 进入 WiFi 配网 | 同左 |
| 单击 · 其他状态 | `ToggleChatState()`；上报 `button_click` | 同左 |
| 长按 | **3 s** 清除 MQTT/WebSocket 协议配置并重启（保留 WiFi/UUID） | **8 s** 同左（带 `Alert` 提示） |
| 双击 | `CONFIG_USE_DEVICE_AEC` 下空闲切换 AEC | 无此处理 |

长按只清 `mqtt`、`websocket` 两个 NVS 命名空间，不整片擦除；重启后重新走激活流程并显示 6 位配对码。
4B 在 Idle/Unknown/Starting/Activating 之外会忽略长按（避免对话中误触）；3.49 当前无此状态判断。

### 4.3 显示与主题

| 功能 | 行为 |
|---|---|
| 公司 logo | 空闲/开机时以全屏 logo 替代默认机器人表情；深色主题用白色版本，浅色用深色版本；远程壁纸激活时隐藏，清壁纸后恢复 |
| 表情位置 | 4B 表情中心上移到屏幕上方 1/4（480 屏 y=120），避免压住居中 logo |
| 预览图 | 远程 `set_image` 默认模式；常驻、可拖动、双击清除 |
| 壁纸 | `mode:"background"`；cover 裁剪、跨主题保留、仅 `clear:true` 清除 |
| 状态栏 | 远程 `set_status` 设置文字与颜色；下发 6 位配对码时也使用状态栏 |
| 主题 | 远程 `set_theme`、三击屏幕、MCP `self.screen.set_theme` 三条入口共用 `LvglThemeManager` |

### 4.4 IMU 运动检测

| 项 | 4B | 3.49 V2 |
|---|---|---|
| 器件 | QMI8658 | QMI8658 |
| 触发状态 | Idle / Notifying | 同左 |
| 动作 | 进入聆听；播报中先 `StopNotificationByUser()` 再进聆听 | 同左 |
| 阈值 | 约 0.20 g | 约 0.25 g |
| 冷却 | 5 s | 同左 |
| 点击抑制 | 700 ms（屏幕/按键后忽略运动） | 同左 |
| 远程开关 | `set_imu`，持久化 NVS `imu/enabled` | 同左 |

实现：[`4B imu_monitor.h`](../main/boards/waveshare/esp32-s3-touch-lcd-4b/imu_monitor.h)、
[`3.49 imu_monitor.h`](../main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/imu_monitor.h)。

### 4.5 远程播报

| 能力 | 说明 |
|---|---|
| 启动 | `notify`，仅 Idle；URL 直链 Ogg Opus；可带字幕与 `id` |
| 回执 | `reply` 的 `success` 表示"已接受并开始播放"，不代表播放成功 |
| 远程取消 | `abort_notify`，仅 Notifying；立即静音，`outcome=cancelled` |
| 本地停止 | 屏幕双击、BOOT 键、IMU 手势；`outcome=user_stopped` |
| 结果上报 | `notify_result` 带 `id/outcome/duration_ms`；QoS 0，离线不补发 |
| 打断 | 唤醒词等转 connecting/listening/speaking 时为 `aborted`；转 activating/upgrading 时为 `interrupted` |

### 4.6 启动、激活与升级

| 功能 | 行为 |
|---|---|
| 激活码 | `ShowActivationCode()` 把 6 位数字放状态栏，串口同步打印 |
| 快速启动 | NVS 已有 MQTT/WebSocket 配置时跳过 `CheckNewVersion()`，只 `MarkCurrentVersionValid()`，避免云端不可达时卡启动 |
| 自动 OTA | 上述快速路径下不再自动检查新版本；`self.upgrade_firmware` 仍可手动升级 |
| 重新绑定 | BOOT 长按清除协议配置后重启，重新走激活流程 |
| RTC 校时 | PCF85063 开机读回并同步系统时间（4B / 3.49 均接） |

### 4.7 唤醒词与音频

| 项 | 4B | 3.49 V2 |
|---|---|---|
| 唤醒词 | “你好，邵怡”（`ni hao shao yi`，阈值 20） | “你好，小威”（`ni hao xiao wei`，阈值 20） |
| 方案 | MultiNet + 自定义唤醒词，关闭默认 WakeNet | 同左 |
| AEC | `CONFIG_USE_DEVICE_AEC=y`；仅 Listening/Speaking/Notifying 开启 | 同左 |
| 唤醒词运行 | 空闲态持续运行，不受 AEC 门控影响 | 同左 |

换唤醒词必须同时更新 app 与 `generated_assets.bin`，否则唤醒词与模型不匹配。

### 4.8 网络、电源与背光

| 功能 | 行为 |
|---|---|
| 网络信息 | `hello` 与 `query_status` 上报 `ip/ssid/rssi`；`WifiBoard` 实现，蜂窝板型为空 |
| 省电 | `toggle_power_save` 仅 Idle 可切 `LOW_POWER` / `PERFORMANCE` |
| 背光持久化 | `set_brightness` 写入 NVS，重启保留 |
| 背光守护 | 3.49 V2 每秒回读 PWM，发现丢失则重写（`ReapplyBrightness`） |
| 状态镜像 | 每次状态变化发布 `session_update`（仅控制通道在线时） |

---

## 5. 与上游的差异边界

| 文件/模块 | 定制内容 |
|---|---|
| `main/control_mqtt.{h,cc}` | 新增第二路 MQTT 控制通道、指令处理、图片任务、播报归因 |
| `main/application.{h,cc}` | 启动控制通道、状态镜像、事件上报、AEC 门控、`StartListening(mode)`、`StopNotificationByUser()`、快速启动 |
| `main/boards/common/board.h` | 网络信息、IMU 可选接口 |
| `main/display/display.h` 及 `lcd_display`/`lvgl_display` | 状态栏颜色、预览常驻、壁纸、诊断查询 |
| `main/boards/common/backlight.{h,cc}` | `ReapplyBrightness()` 背光回读修复 |
| `main/audio/*` | `SetAecRequired()` 状态门控、任务栈放 PSRAM、解码丢包统计 |
| `main/device_state_str.h` | 新增 `DeviceStateToString()` |
| `main/Kconfig.projbuild` | `Control MQTT (second broker)` 配置菜单 |
| `main/boards/waveshare/esp32-s3-touch-lcd-4b/*` | 4B 板级：ST7701、GT911、ES8311/ES7210、IMU、RTC、手势、logo |
| `main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/*` | 3.49 V2 板级：AXS15231B、TCA9554、IMU、RTC、手势、logo、背光守护 |
| `main/mcp_server.cc` | **无改动**（上游标准工具） |

---

## 6. 约束与注意事项

1. **线程模型**：控制通道的 MQTT 回调只做 JSON 解析，所有设备变更通过
   `Application::Schedule()` 投递到主任务；不要在回调里直接操作状态机/显示。
2. **状态约束**：`mcp`、`notify`、`toggle_wake_word`、`toggle_power_save` 仅 Idle；
   `abort_notify` 仅 Notifying；`abort` 仅 Connecting/Listening/Speaking；
   `listen stop` 任意，`listen start` 仅 Idle/Notifying。
3. **参数名**：`set_volume` / `set_brightness` 用 `level`，不是 `volume` / `brightness`。
4. **`reply` 无 `device_id`**：服务端靠 topic 区分设备；其余 5 类上行消息均带 `device_id`。
5. **QoS 0**：事件和 `notify_result` 都可能丢失，服务端需做超时兜底与幂等。
6. **能力可选**：`Board::GetDisplay()/GetBacklight()/GetCamera()` 可能为空，调用方必须先判空；
   `set_image` 等指令在能力缺失时回 `success=false`。
7. **图片内存**：1 MP RGB565 约 2 MB PSRAM；PNG 解码峰值更高，大图优先 JPEG。
8. **明文 MQTT**：默认 21883 不加密，凭据在网络上可见；生产建议 8883 + TLS 或内网链路。
9. **板级差异**：4B 空闲双击进聆听、长按 3 s；3.49 空闲双击仅清图、长按 8 s；
   两者固件不通用。

---

## 7. 扩展检查清单

新增一条控制指令时：

1. `ControlMqtt::DispatchCommand()` 增加分支与状态约束；
2. `control_mqtt.h` 增加 `HandleXxx()` 声明，`control_mqtt.cc` 实现；
3. 所有路径保证恰好一次 `SendReply()`，异步结果另发上行消息；
4. 更新 [`control-mqtt-api.md`](./control-mqtt-api.md)（字段、状态、错误码、示例）；
5. 更新 `scripts/control_mqtt_monitor.py` 的自测/探针逻辑（如适用）；
6. 若涉及新状态或新事件，补 `DeviceStateToString()` / `ReportUserEvent()` 调用点。

新增板级能力时：

1. 在 `Board` / `Display` / `Backlight` 基类加**带默认实现**的虚函数；
2. 仅在实际支持的板级覆盖，调用方判空/看返回值；
3. 用 Kconfig 或板级宏保护资源相关代码；
4. 在本文 §4 与板级文档补能力矩阵。

新增 MCP 工具时：

1. 在 `AddCommonTools()`（AI 可见）或 `AddUserOnlyTools()`（仅用户）注册；
2. 声明参数范围/长度，避免无界输入；
3. 更新 [`mcp-protocol.md`](./mcp-protocol.md) 与对接说明。

---

## 8. 代码与文档索引

| 内容 | 位置 |
|---|---|
| 控制通道实现 | [`main/control_mqtt.cc`](../main/control_mqtt.cc)、[`main/control_mqtt.h`](../main/control_mqtt.h) |
| 控制通道报文速查 | [`docs/control-mqtt-api.md`](./control-mqtt-api.md) |
| 控制通道设计与历史 | [`docs/control-mqtt.md`](./control-mqtt.md) |
| 播报闭环设计 | [`docs/control-mqtt-notify-feedback.md`](./control-mqtt-notify-feedback.md) |
| MCP 工具实现 | [`main/mcp_server.cc`](../main/mcp_server.cc) |
| MCP 协议 | [`docs/mcp-protocol.md`](./mcp-protocol.md) |
| Board 接口 | [`main/boards/common/board.h`](../main/boards/common/board.h) |
| Display 接口 | [`main/display/display.h`](../main/display/display.h) |
| Application 接口 | [`main/application.h`](../main/application.h) |
| 4B 板级 | [`main/boards/waveshare/esp32-s3-touch-lcd-4b/`](../main/boards/waveshare/esp32-s3-touch-lcd-4b/) |
| 3.49 V2 板级 | [`main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/`](../main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/) |
| 项目总览 | [`PROJECT_OVERVIEW.md`](../PROJECT_OVERVIEW.md) |
