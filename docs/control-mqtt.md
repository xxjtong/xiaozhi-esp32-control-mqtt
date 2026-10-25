# 第二路 MQTT 控制通道（ControlMqtt）

本文档记录本项目相对上游 `xiaozhi-esp32` 的全部定制内容：新增的第二路 MQTT 控制通道、为实现它所做的核心代码改动、构建配置修复，以及历次修改与验证记录。

## 1. 背景与目标

设备原有的 MQTT 通道用于 xiaozhi.me 官方协议（会话、UDP 音频、MCP），不应被外部系统直接驱动。本项目新增一条**独立的控制平面**，用于远程控制设备：

- 零侵入：不修改 `MqttProtocol` / `WebSocketProtocol`，不影响官方 UDP 音频通道和会话生命周期
- 独立连接：第二个 MQTT 客户端（`connect_id=1`，官方通道是 `0`），连到自建 broker
- 状态隔离：配置存放在独立 NVS 命名空间 `control_mqtt`
- 线程安全：所有设备状态变更通过 `Application::Schedule()` 调度到主任务执行

## 2. 文件清单

| 文件 | 说明 |
|---|---|
| `main/control_mqtt.h` | `ControlMqtt` 类声明 |
| `main/control_mqtt.cc` | 连接管理、JSON 解析、指令处理、图片下载任务 |
| `main/Kconfig.projbuild` | `Control MQTT (second broker)` 配置菜单；Kconfig 缺失键占位 |
| `main/application.h` | 引入 `ControlMqtt` 成员；`StartNotification` 移至 public |
| `main/application.cc` | 启动初始化、状态变更镜像；断网时不拆除控制通道 |
| `main/device_state.h` | 新增 `DeviceStateToString()` |
| `main/CMakeLists.txt` | 注册 `control_mqtt.cc` |
| `sdkconfig.defaults` | 删除 3 个不可见配置项（见 6.2） |

## 3. 架构与线程模型

```
                 ┌──────────────────────────────────────────┐
                 │  Main task (Application::Run)            │
                 │  - DeviceStateMachine                    │
                 │  - 指令执行 / LVGL 更新 / 回复发送        │
                 └───────▲──────────────────────────┬───────┘
                         │ Schedule()               │ 状态变更
                         │                          ▼
   ┌─────────────┐   ┌───┴──────────────┐   ┌──────────────────┐
   │ ctrl_mqtt   │   │ Application 队列 │   │ ControlMqtt      │
   │ (连接任务)  │──▶│                  │◀──│ OnMessage / 状态  │
   └─────────────┘   └──────────────────┘   └──────────────────┘
          │ Connect()（最多 10s）                    ▲
          ▼                                          │ Publish
   ┌─────────────┐            ┌──────────────────────┴───┐
   │ esp-mqtt    │───────────▶│ mqtt.virsical.cn:21883   │
   │ 自动重连    │            │ (明文 TCP MQTT)          │
   └─────────────┘            └──────────────────────────┘
```

线程约定：

- **MQTT 任务**：只做 JSON 解析，然后 `Schedule()` 到主任务
- **连接任务 `ctrl_mqtt`**：只负责阻塞式的 `Connect()`，不触碰设备状态
- **主任务**：执行所有指令、读写设备状态、更新 LVGL、发送回复
- **图片下载任务 `ctrl_image`**：HTTP 下载（不阻塞主循环），下载完成后把数据交给主任务做解码与显示

## 4. 配置

### 4.1 编译期默认值（Kconfig）

`menuconfig → Xiaozhi Assistant → Control MQTT (second broker)`

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `CONFIG_CONTROL_MQTT_ENDPOINT` | `mqtt.virsical.cn:21883` | `host:port`；`8883` 走 TLS，其它端口走明文 TCP；留空则完全关闭控制通道 |
| `CONFIG_CONTROL_MQTT_USERNAME` | `admin` | 用户名 |
| `CONFIG_CONTROL_MQTT_PASSWORD` | `public` | 密码 |

### 4.2 NVS 覆盖（优先级更高）

命名空间 `control_mqtt`，键名如下，存在即覆盖 Kconfig 默认值：

| 键 | 对应配置 |
|---|---|
| `endpoint` | broker 地址 |
| `client_id` | MQTT 客户端 ID |
| `username` / `password` | 认证信息 |
| `subscribe_topic` | 指令订阅 topic |
| `reply_topic` | 回复/广播 topic |

### 4.3 自动推导

- `client_id` 缺省：`xiaozhi-<UUID 后 8 位>`
- `subscribe_topic` 缺省：`xiaozhi/<设备UUID>/cmd`
- `reply_topic` 缺省：`xiaozhi/<设备UUID>/reply`

未配置 `endpoint` 时不创建任何任务，零开销。

## 5. 协议

### 5.1 上行消息（设备 → `reply_topic`）

| type | 触发时机 | 字段 |
|---|---|---|
| `hello` | 连接成功（含重连） | `device_id`、`status`（当前设备状态）、`version`（固件版本，取自 `esp_app_get_description`）、`ip`（可选，内网地址） |
| `goodbye` | 主动释放连接前 | `device_id` |
| `session_update` | 设备状态变化 | `device_id`、`status` |
| `reply` | 每条指令处理完 | `command_type`、`success`、`device_state`、`extra`（可选） |
| `event` | 本地用户操作（按键/手势/姿态/唤醒） | `device_id`、`event`、`value`（可选）、`state`、`uptime_ms`、`seq` |

### 5.2 下行指令（`subscribe_topic`）

| type | 关键字段 | 状态约束 | 说明 |
|---|---|---|---|
| `listen` | `state`: `start` / `stop` | `start`: Idle / Notifying；`stop`: 任意 | 远程唤醒或停止收音 |
| `abort` | `reason`: 可选 `wake_word_detected` | Connecting / Listening / Speaking | 打断当前回合；不可用时回 `success=false` |
| `mcp` | `payload`: JSON object | Idle | 透传给 MCP 管道 |
| `system` | `command`: `reboot` | 任意 | 先发回复再重启 |
| `set_image` | `url`，或 `clear: true` 清除，可选 `mode` | 需 LVGL 显示 | 下载图片并显示（文件 ≤4MB，分辨率 ≤1280×1280 且 ≤1MP），支持 JPEG / PNG。`mode` 可选：`preview`（默认，浮层居中显示，可拖动，双击清除）或 `background`（桌面壁纸，cover 居中裁剪铺满全屏，跨主题切换保留；JPEG 直接用 RGB565，PNG 在显示层解码为 RGB565）。`clear` 同时清除预览与壁纸。服务端规格建议见 6.3 |
| `set_theme` | `theme`: `light` / `dark` | 需带主题的显示 | 与三击屏幕切换走同一条路径（`LvglThemeManager`） |
| `set_volume` | `level`: 0–100 | 需音频 Codec | 写入 NVS 持久化 |
| `set_brightness` | `level`: 0–100 | 需 Backlight | 持久化到 NVS |
| `toggle_wake_word` | `enable`: bool | Idle | 开关唤醒词检测 |
| `notify` | `audio_url`、`subtitles[]` | Idle | 远程播报（复用通知播放器） |
| `set_status` | `text`、可选 `color` | 需显示 | 状态栏文本；`color` 支持颜色名（`red`/`green`/`blue`/`yellow`/`orange`/`white`/`black`/`cyan`/`purple`/`gray`）或 `#RRGGBB` |
| `toggle_power_save` | `enable`: bool | Idle | 低功耗 / 性能模式 |
| `set_imu` | `enable`: bool | 任意 | 开关 IMU 运动检测，持久化到 NVS `imu/enabled`；无 IMU 板型回 `success=false` |
| `query_status` | — | 任意 | `extra` 返回 `device_state`、`battery`、`charging`、`temperature`、`ip`、`ssid`、`rssi`、`theme`、`volume`、`brightness`、`preview_active`、`wallpaper_active`、`imu_enabled`、`wake_word_running`、`mqtt_connected`（有值才带） |

状态不满足时返回 `success=false`，不会静默吞掉指令。

`set_status` 示例（可选颜色，省略则用当前主题色）：

```json
{"type": "set_status", "text": "这是一条较长的滚动文字，超过十六个汉字会循环滚动", "color": "red"}
{"type": "set_status", "text": "橙色文字", "color": "#FFA500"}
```

颜色只作用于顶部状态行 `status_label_`；下一次状态变化或空闲时每分钟的时钟刷新会恢复主题色并替换文本。

`reply` 示例：

```json
{
  "type": "reply",
  "command_type": "query_status",
  "success": true,
  "device_state": "idle",
  "extra": {
    "device_state": "idle",
    "battery": 87,
    "charging": false,
    "temperature": 41.5,
    "ip": "192.168.6.55",
    "ssid": "Temi",
    "rssi": -59,
    "theme": "light",
    "volume": 70,
    "brightness": 80,
    "preview_active": false,
    "wallpaper_active": false,
    "imu_enabled": true,
    "wake_word_running": true,
    "mqtt_connected": true
  }
}
```

### 5.3 用户行为事件（上行）

设备把**本地用户操作**以 `type:"event"` 发布到 `reply_topic`，用于区分"远程指令"与"现场操作"：

| event | value | 触发条件 |
|---|---|---|
| `wake_word` | — | 语音唤醒词命中（含打断说话/聆听） |
| `interaction` | — | 进入 Speaking 状态，即一次"用户提问 → 设备回答"的交互 |
| `button_click` | `boot` | BOOT 键短按（开始/结束对话、配网入口） |
| `motion_detected` | — | QMI8658 姿态变化触发（仅 Idle，5 秒冷却） |
| `image_cleared` | `double_click` | 双击清除常驻预览图 |
| `theme_changed` | `light` / `dark` | 三击切换主题 |

示例：

```json
{"type":"event","device_id":"0d0cef91-...","event":"theme_changed","value":"dark","state":"idle","uptime_ms":123456,"seq":7}
{"type":"event","device_id":"0d0cef91-...","event":"motion_detected","state":"idle","uptime_ms":124001,"seq":8}
```

约定：

- 这是**尽力而为**的上报：QoS 0，控制 broker 离线期间的事件直接丢弃，不补发
- `seq` 单调递增，用于判重/排序；`uptime_ms` 可与设备日志对齐
- `state` 是事件发生**当时**的设备状态（idle/connecting/listening/speaking/...）
- 事件只携带元数据，不包含唤醒词文本、语音或屏幕内容
- 对话过程仍由 `session_update` 表示（connecting/listening/speaking/idle），`event` 只说明是谁触发了动作
- 交互次数：每进入一次 `Speaking` 状态就发一条 `interaction`，包含唤醒后的首次回应和后续每一轮问答；
  统计 `interaction` 条数即可，不需要依赖 VAD 或识别用户说了几句话

## 6. 关键实现

### 6.1 连接生命周期

- `Start()` 立即返回：创建客户端、注册回调，再拉起 `ctrl_mqtt` 任务执行阻塞式 `Connect()`（内部等待上限 10s）
- 连接失败时**不销毁客户端**，交给 esp-mqtt 内置 auto-reconnect 持续重试；重连成功后 `OnConnected` 再次触发，自动重新 `Subscribe` 并发送 `hello`
- 断网事件（`HandleNetworkDisconnectedEvent`）**不拆除**控制通道，否则一次 WiFi 抖动会让控制通道死到重启
- `Stop()` 仅在对象销毁时使用：先发 `goodbye` 并等待 50ms 让 esp-mqtt 冲刷发送队列，再断开；用 `lifecycle_mutex_` + `connect_task_running_` 避免在 `Connect()` 等待期间销毁客户端
- `connected_` / `enabled_` / `connect_task_running_` 均为 `std::atomic<bool>`，因为 MQTT 任务与主任务都会读写

### 6.2 图片下发的两阶段处理

1. `ctrl_image` 任务：HTTP 下载到 PSRAM（`heap_caps_malloc`，上限 4MB）；先按文件头解析尺寸
   （JPEG 扫 SOF 段、PNG 读 IHDR），超过 1280×1280 或 100 万像素直接拒绝，避免为超大图分配 RGB565 缓冲；
   若是 JPEG（`FF D8` 开头），再用项目自带的 `jpeg_to_image()` 解码成 RGB565——该构建未启用
   `LV_USE_TJPGD`，LVGL 自身解不了 JPEG
2. 主任务：构造 `LvglAllocatedImage`（JPEG 走带宽高的 RGB565 构造，其它格式交给 LVGL 解码器）、
   再按解码后的实际尺寸做一次超限兜底检查、调用 `SetPreviewImage()`、发送回复；构造过程**包 try/catch**

失败时回复带 `extra.error` 指明具体原因，例如 `http status is not 200`、`image too large (max 4 MB)`、
`image too large (max 1280x1280, 1 MP)`、`empty body`、`download incomplete`、`jpeg decode failed`、
`Failed to get image info`（非图片内容），不再是无声的 `success=false`。
同一时刻只允许一个下载任务（`image_fetch_running_`），并发请求回 `another image download is in progress`，避免多份 4MB 缓冲把 PSRAM 打满。

上游的 `LcdDisplay::SetPreviewImage()` 会在 `PREVIEW_IMAGE_DURATION_MS`（5 秒）后自动隐藏预览图。
远程下发的图片不应闪一下就消失，因此给 `LvglDisplay` 增加了 `CancelPreviewTimeout()`（默认空实现，
`LcdDisplay` 覆写为停掉 `preview_timer_`），应用图片后调用它让图片常驻；
`{"type":"set_image","clear":true}` 通过 `SetPreviewImage(nullptr)` 恢复默认 UI。

注意 `LcdDisplay::SetTheme()` 只改样式、不重建对象树，所以切主题不会清掉图片。
本板型手势：**双击清预览图，三击切主题**；`CustomLcdDisplay::ClearPreviewImage()` 只在存在
常驻预览图时清图并上报 `image_cleared`，`ToggleTheme()` 不再顺带清图。
双击后等待 500ms 确认不是三击才清图；三击会取消待执行的双击并只切主题，因此
“三击”不会顺带清掉图片。壁纸不受双击影响，仍由 `{"type":"set_image","clear":true}` 清除。
`mode=background` 对两种格式都走 `CreateCoverBackground()` 转成 RGB565：JPEG 已由
`jpeg_to_image()` 解好；PNG 则在显示层调用 LVGL lodepng 解为 ARGB8888 后再转 RGB565
（透明像素叠黑底），然后做 cover 裁剪。解码失败、格式不支持或 PSRAM 不足时回
`success=false, extra.error="failed to apply background image"`，不再出现“回成功但屏幕没变”。
PNG 多占一份解码缓冲，1MP 级 PNG 的峰值内存较高，建议壁纸仍控制在 512px 左右。

缓冲区所有权：JPEG 源数据解码后立即释放，解码结果交给 `LvglAllocatedImage` 析构释放；
构造抛异常、无显示设备、对象已销毁等分支自行 `heap_caps_free()`。

显示方式：预览图挂在透明容器 `preview_viewport_` 内，按**屏幕宽度的 80%** 等比缩放
（本板可见区域 640×172，即 512 宽），高度随比例；比屏幕高的图片可在容器内**上下拖动**，
滚动条关闭。缩放在设备端由 LVGL 完成，服务端应尽量下发接近目标尺寸的图片。

### 6.3 图片规格建议（服务端）

设备端只做等比缩放与显示，不做重编码，因此服务端（第三方平台）下发前请按下列规格处理：

| 项目 | 建议值 | 硬限制（超限设备拒绝并回 `extra.error`） |
|---|---|---|
| 宽度 | **512 px**（屏幕 640 的 80%） | ≤1280 px |
| 高度 | 按原图比例，建议 ≤640 px | ≤1280 px |
| 总像素 | ≤30 万 | ≤100 万（1 MP） |
| 文件体积 | ≤1 MB | ≤4 MB |
| 编码格式 | JPEG（baseline）或 PNG | 同左；其它格式依赖 LVGL 解码器 |

要点：

1. 先按 512 宽等比缩放，再压缩到 1 MB 以内（JPEG 质量 80 左右即可）
2. 避免下发原始相机图（4000×3000 约 1200 万像素，解码需 ~24 MB PSRAM，必然失败）
3. 屏幕为超宽比例（640×172），高图也能显示，但需要用户上下拖动查看；重要内容建议放在图片上部
4. 图片会常驻屏幕，直到被新图替换、下发 `{"type":"set_image","clear":true}` 或设备重启

### 6.4 其他细节

- 显示能力视为可选：`set_image` / `set_status` / `set_theme` 都先判空 `Board::GetInstance().GetDisplay()`
- `set_theme` 复用 `LvglThemeManager`（与三击屏幕切换、MCP 的 `self.screen.set_theme` 同一路径），支持 `light` / `dark`
- 设备状态字符串统一由 `DeviceStateToString()` 提供，避免多处重复 switch
- `cJSON_PrintUnformatted()` 的释放一律用 `cJSON_free()`
- `SendReply()` 在未连接时提前返回，同时释放调用方传入的 `extra`，避免泄漏

## 7. 历次修改记录

### 阶段一：新增第二路 MQTT 控制通道

- 新增 `main/control_mqtt.h` / `main/control_mqtt.cc`，实现连接、重连、JSON 解析与下行指令
- `main/application.h`：引入 `control_mqtt.h`，新增 `std::unique_ptr<ControlMqtt> control_mqtt_`
- `main/application.cc`：`InitializeProtocol()` 末尾创建并启动；状态变更时广播 `session_update`
- `main/CMakeLists.txt`：注册 `control_mqtt.cc`

### 阶段二：构建问题修复（Kconfig）

现象：`idf.py build` 报 `Missing required kconfig option after retry.`

依次定位并修复：

1. `CONFIG_NEWLIB_NANO_FORMAT=y`：v6.0 已更名为 `LIBC_NEWLIB_NANO_FORMAT`，且 `LIBC_NEWLIB` 未启用（项目使用 picolibc），配置项不可见 → 从 `sdkconfig.defaults` 删除
2. `CONFIG_LV_USE_CALENDAR_HEADER_ARROW` / `..._DROPDOWN`：依赖已关闭的 `LV_USE_CALENDAR`，不可见 → 删除
3. 仍失败。通过直接调用组件管理器的 `prepare_dep_dirs()` 打印 `KCONFIG_CONTEXT.missed_keys`，定位到根因：
   - 可选依赖 `espressif/esp_h264`（仅 `esp32p4` 引入）的 `idf_component.yml` 的 `if` 子句引用了 `ESP_VIDEO_USE_CUSTOMIZED_ESP_H264_VERSION`
   - 该符号由 `esp_video` 2.5+ 定义，本项目用的是 2.4.1，符号不存在 → 组件管理器返回退出码 10，cmake 重试后报错
   - 修复：在 `main/Kconfig.projbuild` 提供占位 `config ESP_VIDEO_USE_CUSTOMIZED_ESP_H264_VERSION`（默认 `n`，带 prompt 以便写入 `sdkconfig.json`），并在 `sdkconfig.defaults` 临时补一行默认值；后续确认仅靠 Kconfig 默认值即可，已删除该冗余行

### 阶段三：代码审查与缺陷修复

对初版实现逐项审查后修复：

| 问题 | 修复方式 |
|---|---|
| 断网时 `Stop()` 导致控制通道永久失效 | 移除断网时的 `Stop()`，交给 esp-mqtt 自动重连 |
| 自建重连定时器泄漏 / 裸 `this` 回调 / `Stop()` 后仍会回魂重连 | 删除定时器逻辑，改用 esp-mqtt 内置 auto-reconnect |
| `Start()` 同步阻塞主任务最多 10s | 连接放到 `ctrl_mqtt` 独立任务 |
| `set_image` 在主任务同步下载，卡住音频与 UI | 下载移入 `ctrl_image` 任务 |
| 非法图片使 `LvglAllocatedImage` 抛异常导致重启 | 加 try/catch 并释放缓冲区 |
| 无屏板型解引用空显示指针 | `set_status` / `set_theme` 增加判空 |
| `connected_` 数据竞争 | 改为 `std::atomic<bool>` |
| `goodbye` 发送后立即断开，消息丢失 | 断开前等待 50ms |
| `hello` 版本号硬编码、状态固定为 idle | 版本取固件描述，状态取当前 `DeviceState` |
| `set_theme` 空实现却返回成功 | 改为 `success=false` 并附 `extra.error` |
| `SendReply` 提前返回时 `extra` 泄漏 | 提前返回前 `cJSON_Delete(extra)` |
| 三处重复的设备状态字符串 switch | 抽取 `DeviceStateToString()` |
| `free()` 释放 cJSON 字符串 | 统一改为 `cJSON_free()` |
| `reconnect_timer_`、`event_group_handle_` 等死成员 | 删除 |
| `toggle_wake_word` / `toggle_power_save` 非 Idle 时静默生效但回报成功 | 非 Idle 返回 `success=false` |

### 阶段四：接入实际 broker

- 目标 broker：`mqtt.virsical.cn:21883`，账号 `admin` / `public`
- 新增 Kconfig 默认值（见 4.1），`LoadConfig()` 改为「NVS 优先，Kconfig 兜底」
- 用原始 MQTT 3.1.1 CONNECT 报文探测，确认 21883 为**明文 TCP** 且返回 `CONNACK 0 (accepted)`，无需 TLS
- 编译产物大小 `0x2da0c0`，分区余量 28%

### 阶段五：仓库发布

- `git init`（默认分支 `main`），`.gitignore` 已排除 `build/`、`managed_components/`、`sdkconfig`、`dependencies.lock`
- 创建私有仓库 `xxjtong/xiaozhi-esp32-control-mqtt` 并推送
- 本机 `github.com:443` 不可达，remote 改为 SSH over 443：`ssh://git@ssh.github.com:443/xxjtong/xiaozhi-esp32-control-mqtt.git`

### 阶段六：真机全量回归与缺陷修复

对真机（`0d0cef91…1e2d364c`）逐条下发全部指令，用本地 HTTP 素材服务与串口日志交叉验证：

| 发现的问题 | 处理 |
|---|---|
| `play_sound` 把名字当 OGG 字节传给 `PlaySound()`，永远无声 | 上游无「按名字播音效」的能力，删除该指令 |
| `set_theme` 空实现 | 改用 `LvglThemeManager` 查找主题后 `display->SetTheme()`，与桌面手势/MCP 同一路径 |
| `set_image` 收 JPEG 必失败 | 构建未开 `LV_USE_TJPGD`；改为在下载任务里调用项目自带 `jpeg_to_image()` 解成 RGB565 再交给 LVGL |
| `listen stop` 在 listening 时被拒 | 拆分守卫：`stop` 放开到任意状态，`start` 仍限制 Idle/Notifying |
| `abort` 无条件回 `success=true` | 仅在 Connecting/Listening/Speaking 回成功，其余回失败 |
| 图片失败无原因 | 回复增加 `extra.error` |
| 并发 `set_image` 可能同时占满 PSRAM | 增加 `image_fetch_running_` 单飞保护；注意 `vTaskDelete()` 不会执行栈上析构，标志必须在退出前显式清除 |
| 图片 5 秒后自动消失 | 新增 `LvglDisplay::CancelPreviewTimeout()`，应用远程图片后停掉预览定时器；另支持 `clear:true` 恢复 UI |

实测结果（阶段六历史编译产物 `0x2ec3e0`，当时分区余量 26%）：

```
set_theme dark   -> success=true
set_theme light  -> success=true
set_theme foo    -> success=false, extra.error="unknown theme (supported: light, dark)"
set_image jpg    -> success=true        （此前为 false）
set_image png    -> success=true
set_image 404    -> success=false, extra.error="download failed"
play_sound       -> success=false       （指令已删除）
listen start     -> success=true, 状态 idle→connecting→listening
listen stop      -> success=true, 状态回 idle（此前为 false）
abort (listening)-> success=true
abort (idle)     -> success=false       （此前误报成功）
```

### 阶段七：图片显示与尺寸保护

| 发现的问题 | 处理 |
|---|---|
| 图片居中缩小、很小，右侧有滚动条、屏幕可上下拖拽 | 预览控件误用面板原始分辨率 `width_/height_`（172×640），改为旋转后的 `LV_HOR_RES/LV_VER_RES`（640×172） |
| 需要按比例显示并允许浏览长图 | 预览图放入全屏透明容器，按屏宽 80%（512 px）等比缩放；容器只允许垂直滚动、滚动条关闭 |
| 预览容器会吞掉点击事件 | 在 `CustomLcdDisplay::SetupUI()` 里给容器单独注册点击判定，保持「双击清图、三击切主题」 |
| 超大图片可能耗尽 PSRAM | 下载后先按文件头预检尺寸（JPEG SOF / PNG IHDR），主任务解码后再兜底；超过 1280×1280 或 100 万像素回 `extra.error="image too large (max 1280x1280, 1 MP)"` |

服务端（第三方平台）无法改代码，改为在文档 6.3 给出图片规格建议（512 宽、≤1MB、≤30 万像素）。

### 阶段八：状态文字颜色

- `set_status` 新增可选 `color`：支持 10 个颜色名与 `#RRGGBB`，非法值回 `success=false` 及 `extra.error`
- 新增 `Display::SetStatusColor()`（默认空实现）与 `LvglDisplay` 覆写，仅改 `status_label_` 的样式
- `LvglDisplay::SetStatus()` 每次都把颜色恢复为主题色，避免时钟/状态文本继承上一次的远程颜色
- 长文本滚动能力确认：480px 窗口约 16 个汉字，超出后循环滚动，单圈最长 10 秒

### 阶段九：本地用户行为上报

- 新增 `ControlMqtt::BroadcastEvent()` 与 `Application::ReportUserEvent()`，向 `reply_topic` 发布 `type:"event"`
- 接入点：语音唤醒、BOOT 短按、IMU 姿态变化、双击清图、三击切主题
- 所有调用都经 `Application::Schedule()` 回到主任务，避免在按键/IMU/LVGL 任务里直接访问 MQTT
- 事件为 QoS 0 尽力而为，离线丢弃；携带 `seq`、`uptime_ms`、`state` 便于对账，不含文本/语音内容

### 阶段十：交互次数上报

- 不再依赖 VAD 统计"说了几句话"，改为在状态机监听器里统计交互：
  每次 `old_state != Speaking` 且 `new_state == Speaking` 时发布 `interaction`
- 覆盖唤醒后的首次回应和后续每一轮问答；统计 `interaction` 条数即为交互次数
- 事件由 `ReportUserEvent()` 统一排队到主任务发布，QoS 0，离线丢弃
- 未引入额外定时器或音频侧改动，VAD 逻辑保持原样（仅用于 LED/自动停止等原有功能）

### 阶段十一：网络信息上报

- `Board` 新增可选接口 `GetIpAddress()` / `GetWifiSsid()` / `GetWifiRssi()`，默认返回空值
- `WifiBoard` 从 `WifiManager` 取值；`EthernetBoard` 返回缓存的 IP；`DualNetworkBoard` 委托当前板卡
- `hello` 增加 `ip`；`query_status` 的 `extra` 增加 `ip`、`ssid`、`rssi`（有值时才带）
- 蜂窝板型（ML307/ NT26 等）无对应数据时自动省略，不影响旧字段

### 阶段十二：远程桌面壁纸

- `set_image` 增加可选 `mode` 字段：`preview`（默认，原浮层行为）/ `background`（桌面壁纸）
- 壁纸复用项目原有主题背景图机制：`LvglTheme::set_background_image()` 把图装到 light/dark 两个主题上，`SetTheme()` 时由 `lv_obj_set_style_bg_image_src(container_, ...)` 绘制到全屏背景层，UI 元素自然浮在其上
- 新增 `LcdDisplay::CreateCoverBackground()`：最近邻 cover 缩放（按短边铺满、居中裁剪）到 `LV_HOR_RES × LV_VER_RES` 的 RGB565 缓冲，存 PSRAM
- 壁纸跨明暗主题切换保留（同时注册到两个主题）；双击只清预览图、不清壁纸；清壁纸走 `{"type":"set_image","clear":true}`
- 当前为会话级（运行时内存），断电重启后恢复纯色背景；如需持久化可后续存到 SPIFFS

### 阶段十三：手势/诊断/壁纸健壮性

- 手势改为「双击清预览图、三击切主题」：`CustomLcdDisplay` 自行累计 `LV_EVENT_SHORT_CLICKED` 次数（500ms 窗口），第 2 次启动 500ms 定时器，窗口到期才清图并上报 `image_cleared`；第 3 次取消定时器、切主题并上报 `theme_changed`，因此三击不会顺带清图；清图与切主题不再绑定
- `LvglDisplay::SetBackgroundImage()` 改为返回 `bool`；`LcdDisplay` 在转换失败/OOM 时返回 false，控制通道据此回 `success=false` + `extra.error`，修复“壁纸没生效却回成功”
- `mode=background` 支持 JPEG/PNG：JPEG 已在下载任务解为 RGB565；PNG 在显示层用 lodepng 解为 ARGB8888 后转 RGB565（透明像素叠黑底），再 cover 裁剪。解码失败/OOM 回 `success=false`，错误为 `failed to apply background image`
- `query_status` 增加诊断字段：`theme`、`volume`、`brightness`、`preview_active`、`wallpaper_active`、`imu_enabled`、`wake_word_running`，测试脚本无需读串口即可断言
- 新增 `set_imu` 命令：运行时开关 IMU 运动检测，持久化到 NVS `imu/enabled`；`Board` 增加可选 `HasImu/SetImuEnabled/IsImuEnabled` 接口，无 IMU 板型保持默认
- 预览图缩放增加上限：小图不再放大到屏宽 80%，最高保持原尺寸（scale ≤ 256），避免马赛克

### 阶段十四：上游 v2.5.2 局部补丁（路线 A）

保留本 fork 的 control MQTT/目标板定制，只移植以下上游修复：

- MQTT `SendAudio()` 不再持 `channel_mutex_` 发送 UDP；`udp_` 改为 `shared_ptr`，发送期间对象不会被 `CloseAudioChannel()` 释放
- WebSocket `ParseServerHello()` 先校验 `transport` 是字符串，避免非字符串字段导致 `strcmp(nullptr, ...)`
- `LvglTheme::ParseColor()` 要求颜色至少 7 字符，避免 `#fff` 触发 `substr()` 异常
- 自定义消息的 `cJSON_PrintUnformatted()` 改用 `CJsonStringUniquePtr`，修复每次消息的字符串泄漏
- OGG demuxer 补齐末尾零长度 lace 的处理
- OTA `ParseVersion()` 改用 `std::from_chars`，非法版本返回空结果而不是抛 `std::stoi` 异常

暂未整体升级到 v2.5.2，因此 MCP 错误码/`max_length`、C++ 异常关闭、`NetworkResult`、epd 公共类等架构改动未合入。

## 8. 构建与验证

```sh
source /path/to/esp-idf/export.sh
idf.py build
```

当前结果（`waveshare/esp32-s3-touch-lcd-3.49-v2`，ESP-IDF v6.0.2）：

```
xiaozhi.bin binary size 0x2eef20 bytes.
Smallest app partition is 0x3f0000 bytes. 0x1010e0 bytes (25.5%) free.
```

已验证：

- 编译通过，`control_mqtt.cc` / `application.cc` 强制重编译无 warning（`-Wall -Wextra -Werror`）
- broker 联通性与认证（CONNACK 0）
- 真机全量指令回归（见阶段六）：音量/亮度/省电经串口日志确认生效，主题/图片/会话控制在真机上验证通过
- 断网重连、重启后自动上线（`hello` 恢复）

尚未验证（需现场目视/试听或对端配合）：

1. `set_image` 的屏幕实际观感（缩放、色彩）
2. `set_status` 的文字显示、`toggle_wake_word` 的唤醒行为
3. `mcp` 在 Idle 下是否真正送达（依赖协议通道是否保持）
4. 图片解码期间音频播放是否仍流畅（JPEG 解码在独立任务，理论不影响）
5. 阶段十三改动：双击清图 / 三击切主题的实际手感、JPEG 壁纸显示、`set_imu` 开关、`query_status` 新诊断字段

## 9. 已知限制

- `set_theme` 依赖显示实现注册的主题（LCD 为 `light` / `dark`，部分 OLED 只有 `dark`）；未注册的名字回 `success=false`
- `set_image` 的图片会常驻屏幕，直到被新图替换、`{"clear":true}` 清除，或设备重新渲染 UI；`EmoteDisplay` / `OttoEmojiDisplay` 未覆写 `CancelPreviewTimeout()`，仍沿用各自行为
- `set_image` 的 `mode=background` 支持 JPEG/PNG；PNG 需要额外的 ARGB8888 解码缓冲，1MP 级 PNG 可能因 PSRAM 不足回 `success=false`，壁纸建议控制在 512px 左右
- `set_image` 有硬性上限：文件 ≤4MB、分辨率 ≤1280×1280 且 ≤100 万像素；超限直接回 `success=false`，无法通过配置调整
- IMU 运动检测阈值固定 0.1g，静止放置偶发误触发；可用 `set_imu` 关闭，重启后仍保持 NVS 中的开关状态
- `notify` 回复在播放真正开始前就返回成功（播放失败通过日志体现）
- `play_sound` 已移除：上游只能按 OGG 字节播放，没有「按名字播音效」的接口
- 明文 MQTT（非 8883 端口）不加密，凭据在网络上可见；生产环境建议换 8883 + TLS 或使用内网链路
- Kconfig 中的默认凭据会随源码分发，公开仓库前应清空并改由 NVS 注入
