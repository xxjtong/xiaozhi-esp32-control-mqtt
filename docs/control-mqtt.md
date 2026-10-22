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
| `hello` | 连接成功（含重连） | `device_id`、`status`（当前设备状态）、`version`（固件版本，取自 `esp_app_get_description`） |
| `goodbye` | 主动释放连接前 | `device_id` |
| `session_update` | 设备状态变化 | `device_id`、`status` |
| `reply` | 每条指令处理完 | `command_type`、`success`、`device_state`、`extra`（可选） |

### 5.2 下行指令（`subscribe_topic`）

| type | 关键字段 | 状态约束 | 说明 |
|---|---|---|---|
| `listen` | `state`: `start` / `stop` | Idle / Notifying | 远程唤醒或停止收音 |
| `abort` | `reason`: 可选 `wake_word_detected` | 任意 | 打断当前说话 |
| `mcp` | `payload`: JSON object | Idle | 透传给 MCP 管道 |
| `system` | `command`: `reboot` | 任意 | 先发回复再重启 |
| `set_image` | `url` | 需 LVGL 显示 | 下载图片（≤4MB）并全屏预览 |
| `set_theme` | `theme` | 任意 | 当前返回 `success=false`（板级主题无通用注册表） |
| `set_volume` | `level`: 0–100 | 需音频 Codec | 写入 NVS 持久化 |
| `set_brightness` | `level`: 0–100 | 需 Backlight | 持久化到 NVS |
| `toggle_wake_word` | `enable`: bool | Idle | 开关唤醒词检测 |
| `notify` | `audio_url`、`subtitles[]` | Idle | 远程播报（复用通知播放器） |
| `set_status` | `text` | 需显示 | 状态栏/通知文本 |
| `toggle_power_save` | `enable`: bool | Idle | 低功耗 / 性能模式 |
| `query_status` | — | 任意 | `extra` 返回 `device_state`、`battery`、`charging`、`temperature`、`mqtt_connected` |
| `play_sound` | `sound` | 任意 | 播放内置音效 |

状态不满足时返回 `success=false`，不会静默吞掉指令。

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
    "mqtt_connected": true
  }
}
```

## 6. 关键实现

### 6.1 连接生命周期

- `Start()` 立即返回：创建客户端、注册回调，再拉起 `ctrl_mqtt` 任务执行阻塞式 `Connect()`（内部等待上限 10s）
- 连接失败时**不销毁客户端**，交给 esp-mqtt 内置 auto-reconnect 持续重试；重连成功后 `OnConnected` 再次触发，自动重新 `Subscribe` 并发送 `hello`
- 断网事件（`HandleNetworkDisconnectedEvent`）**不拆除**控制通道，否则一次 WiFi 抖动会让控制通道死到重启
- `Stop()` 仅在对象销毁时使用：先发 `goodbye` 并等待 50ms 让 esp-mqtt 冲刷发送队列，再断开；用 `lifecycle_mutex_` + `connect_task_running_` 避免在 `Connect()` 等待期间销毁客户端
- `connected_` / `enabled_` / `connect_task_running_` 均为 `std::atomic<bool>`，因为 MQTT 任务与主任务都会读写

### 6.2 图片下发的两阶段处理

1. `ctrl_image` 任务：HTTP 下载到 PSRAM（`heap_caps_malloc`，上限 4MB），失败直接结束
2. 主任务：构造 `LvglAllocatedImage`（**包 try/catch**，非法图片会抛异常）、调用 `SetPreviewImage()`、发送回复

缓冲区所有权：正常路径交给 `LvglAllocatedImage` 析构释放；构造抛异常、无显示设备、对象已销毁等分支自行 `heap_caps_free()`。

### 6.3 其他细节

- 显示能力视为可选：`set_image` / `set_status` / `set_theme` 都先判空 `Board::GetInstance().GetDisplay()`
- 设备状态字符串统一由 `DeviceStateToString()` 提供，避免多处重复 switch
- `cJSON_PrintUnformatted()` 的释放一律用 `cJSON_free()`
- `SendReply()` 在未连接时提前返回，同时释放调用方传入的 `extra`，避免泄漏

## 7. 历次修改记录

### 阶段一：新增第二路 MQTT 控制通道

- 新增 `main/control_mqtt.h` / `main/control_mqtt.cc`，实现连接、重连、JSON 解析与 14 条指令
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

## 8. 构建与验证

```sh
source /path/to/esp-idf/export.sh
idf.py build
```

当前结果（`waveshare/esp32-s3-touch-lcd-3.49-v2`，ESP-IDF v6.0.2）：

```
xiaozhi.bin binary size 0x2da0c0 bytes.
Smallest app partition is 0x3f0000 bytes. 0x115f40 bytes (28%) free.
```

已验证：

- 编译通过，`control_mqtt.cc` / `application.cc` 强制重编译无 warning（`-Wall -Wextra -Werror`）
- broker 联通性与认证（CONNACK 0）

尚未验证（需硬件/真机联调）：

1. 真实 broker 下的 `hello` / `reply` / `session_update` 收发
2. WiFi 断开恢复后控制通道自动重连的时序
3. `set_image` 的实际显示效果（缩放依赖解码出的宽高）
4. `mcp` 在 Idle 下是否真正送达（依赖协议通道是否保持）
5. 图片下载期间音频播放是否仍流畅

## 9. 已知限制

- `set_theme` 未实现：主题对象由具体板型定义，项目内没有通用注册表；如需支持，应在板级代码中建立「主题名 → `Theme*`」映射后再接入
- `notify` 回复在播放真正开始前就返回成功（播放失败通过日志体现）
- 明文 MQTT（非 8883 端口）不加密，凭据在网络上可见；生产环境建议换 8883 + TLS 或使用内网链路
- Kconfig 中的默认凭据会随源码分发，公开仓库前应清空并改由 NVS 注入
