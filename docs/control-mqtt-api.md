# 第二路 MQTT 控制通道 · 消息与功能速查

面向**对接方 / 服务端**的接口清单。实现细节、改动历史与已知限制见
[`control-mqtt.md`](./control-mqtt.md)，播报闭环的设计稿见
[`control-mqtt-notify-feedback.md`](./control-mqtt-notify-feedback.md)。

代码位置：`main/control_mqtt.{h,cc}`（类 `ControlMqtt`）。

---

## 1. 通道总览

```
服务端 ──订阅──▶ xiaozhi/<device_id>/reply      设备 → 服务端（状态、回执、事件）
服务端 ──发布──▶ xiaozhi/<device_id>/cmd        服务端 → 设备（下行指令）
```

| 项 | 值 |
|---|---|
| 用途 | 纯控制平面，与 xiaozhi.me 官方协议通道完全独立 |
| 客户端 | 独立 MQTT 客户端（`connect_id=1`，官方通道为 `0`） |
| 默认 broker | `mqtt.virsical.cn:21883`（明文 TCP；`8883` 走 TLS） |
| 默认凭据 | `admin` / `public` |
| `device_id` | 设备 UUID（`Board::GetInstance().GetUuid()`） |
| `client_id` | `xiaozhi-<UUID 后 8 位>` |
| Keep Alive | 120 s |
| QoS | 0（尽力而为） |
| 编码 | UTF-8 JSON，压缩无空格（`cJSON_PrintUnformatted`） |
| 配置来源 | NVS 命名空间 `control_mqtt` 优先，Kconfig 兜底 |

**与官方通道的关系**：零侵入。第二路不参与 xiaozhi.me 会话、不传 UDP 音频；设备端
仍由官方通道负责 AI 对话。两条链路可同时在线。

---

## 2. Topic 约定

| 方向 | 默认 Topic | NVS 覆盖键 |
|---|---|---|
| 下行（订阅） | `xiaozhi/<device_id>/cmd` | `subscribe_topic` |
| 上行（发布） | `xiaozhi/<device_id>/reply` | `reply_topic` |

NVS 命名空间 `control_mqtt` 可用键：`endpoint`、`client_id`、`username`、`password`、
`subscribe_topic`、`reply_topic`。未配置 `endpoint` 时通道完全不启用。

> **注意**：respond 消息 `reply` **不含 `device_id` 字段**——服务端靠 topic 中的
> `device_id` 区分来源设备。其余 5 类上行消息均携带 `device_id`。

---

## 3. 上行消息（设备 → 服务端）

共 6 类，全部发布到 `reply_topic`。

| type | 触发时机 | 完整字段 |
|---|---|---|
| `hello` | 连接成功，**含每次重连** | `type`、`device_id`、`status`、`version`、`ip`（有值时） |
| `goodbye` | 主动释放连接前 | `type`、`device_id` |
| `session_update` | 设备状态每次变化 | `type`、`device_id`、`status` |
| `reply` | 每条下行指令处理完 | `type`、`command_type`、`success`、`device_state`、`id`（有值时）、`extra`（有值时） |
| `notify_result` | 播报离开 `notifying` 状态 | `type`、`device_id`、`id`（有值时）、`outcome`、`duration_ms` |
| `event` | 本地用户操作（按键/手势/唤醒/交互） | `type`、`device_id`、`event`、`value`（有值时）、`state`、`uptime_ms`、`seq` |

```json
{"type":"hello","device_id":"0d0cef91-...","status":"idle","version":"2.4.2.1","ip":"192.168.6.55"}
{"type":"session_update","device_id":"0d0cef91-...","status":"listening"}
{"type":"reply","command_type":"set_volume","success":true,"device_state":"idle"}
{"type":"notify_result","device_id":"0d0cef91-...","id":"broadcast-12345","outcome":"finished","duration_ms":5416}
{"type":"event","device_id":"0d0cef91-...","event":"theme_changed","value":"dark","state":"idle","uptime_ms":123456,"seq":7}
```

### 3.1 `status` / `device_state` 取值

`unknown` · `starting` · `wifi_configuring` · `idle` · `connecting` · `listening` ·
`speaking` · `notifying` · `upgrading` · `activating` · `audio_testing` · `fatal_error`

### 3.2 `event` 事件清单

| event | value | 触发条件 |
|---|---|---|
| `wake_word` | — | 语音唤醒词命中（含打断说话/聆听/播报） |
| `interaction` | — | 每次进入 `speaking`，即一轮"提问→回答"完成 |
| `button_click` | `boot` | BOOT 键短按 |
| `motion_detected` | — | IMU 姿态变化（Idle / Notifying，5 s 冷却；点击后 700 ms 抑制） |
| `notify_stopped` | `double_click` | 播报中屏幕双击停止播报 |
| `image_cleared` | `double_click` | 空闲时双击清除预览图 |
| `theme_changed` | `light` / `dark` | 三击切换主题 |

约定：`seq` 单调递增用于判重/排序；`state` 是事件发生**当时**的状态；
事件 QoS 0、离线期间直接丢弃不补发；统计 `interaction` 条数即得交互次数。

### 3.3 `notify_result` 的 outcome

判定顺序自上而下，命中即止：

| outcome | 含义 | 触发来源 |
|---|---|---|
| `cancelled` | 远程取消 | 下行 `abort_notify`；播报中下发 `listen` |
| `user_stopped` | 本地用户停止 | 屏幕双击、BOOT 键、IMU 手势 |
| `aborted` | 被接管 | 唤醒词打断（转 connecting / listening / speaking） |
| `interrupted` | 被系统动作打断 | 转 activating / upgrading（如 OTA） |
| `finished` | 音频播完**或播放失败** | 播放完成回调 |

> `finished` 不区分"播完"与"失败"。服务端需用 `duration_ms` 做启发式判断
> （明显短于预期即疑似失败）。远程标记优先于本地标记，避免远程指令被误报成机身手势。

---

## 4. 下行指令（服务端 → 设备）

共 15 条，发布到 `subscribe_topic`。**所有指令都必须带 `type` 字段**。

| # | type | 必填字段 | 可选字段 | 前置状态 | 功能 |
|---|---|---|---|---|---|
| 1 | `listen` | `state`: `start` / `stop` | — | `start`: idle / notifying；`stop`: 任意 | 远程开始/停止聆听。`start` 走 `ToggleChatState()`（idle→聆听，notifying→停播报） |
| 2 | `abort` | — | `reason`: `wake_word_detected` | connecting / listening / speaking | 打断当前对话回合 |
| 3 | `mcp` | `payload`: JSON object | — | idle | 透传给设备侧 MCP 管道 |
| 4 | `system` | `command`: `reboot` | — | 任意 | 重启设备（先回执再重启） |
| 5 | `set_image` | `url` 或 `clear:true` | `mode`: `preview` / `background` | 需 LVGL 显示 | 远程下发图片，见 §4.1 |
| 6 | `set_theme` | `theme`: `light` / `dark` | — | 需带主题的显示 | 切换明暗主题 |
| 7 | `set_volume` | `level`: 0–100 | — | 需音频 Codec | 设置音量，写入 NVS 持久化 |
| 8 | `set_brightness` | `level`: 0–100 | — | 需 Backlight | 设置背光亮度，持久化到 NVS |
| 9 | `toggle_wake_word` | — | `enable`: bool（默认 true） | idle | 开关唤醒词检测 |
| 10 | `notify` | `audio_url` | `subtitles[]`、`id` | idle | 远程语音播报，见 §4.2 |
| 11 | `abort_notify` | — | — | notifying | 取消正在进行的播报，立即静音 |
| 12 | `set_status` | `text` | `color` | 需显示 | 设置状态栏文本（支持滚动），见 §4.3 |
| 13 | `toggle_power_save` | — | `enable`: bool（默认 true） | idle | 低功耗 / 性能模式切换 |
| 14 | `set_imu` | — | `enable`: bool（默认 true） | 任意 | 开关 IMU 运动检测，持久化到 NVS `imu/enabled` |
| 15 | `query_status` | — | — | 任意 | 查询设备全量状态，见 §4.4 |

**通用约定**：

- 状态不满足时返回 `success=false`，不会静默吞掉指令
- 未知 `type` 也回 `reply`，`command_type` 为原值、`success=false`
- 每条指令**有且仅有一次** `reply`；异步动作（如 `notify`、`set_image`）的结果由
  其它上行消息补充
- 非 JSON 或缺少 `type` 的消息被丢弃，不产生回执

### 4.1 `set_image` 详解

```json
{"type":"set_image","url":"https://.../cover.jpg"}                  // 浮层预览（默认）
{"type":"set_image","url":"https://.../wall.jpg","mode":"background"} // 桌面壁纸
{"type":"set_image","clear":true}                                    // 清除预览 + 壁纸
```

| 模式 | 行为 | 清除方式 |
|---|---|---|
| `preview`（默认） | 居中浮层，按屏宽 80% 等比缩放，长图可上下拖动。**常驻显示**（已停用上游 5 s 自动隐藏） | 双击屏幕，或 `clear:true` |
| `background` | 桌面壁纸，cover 居中裁剪铺满全屏，UI 元素浮于其上，**跨主题切换保留** | 仅 `clear:true` |

限制与规格建议：

| 项 | 硬限制（超限即拒） | 服务端建议 |
|---|---|---|
| 分辨率 | ≤1280×1280 且 ≤100 万像素 | 4B 板型 384 宽；3.49 板型 512 宽 |
| 文件体积 | ≤4 MB | ≤1 MB（JPEG 质量 80） |
| 格式 | JPEG（baseline）/ PNG | 同左 |

同一时刻只允许一个下载任务，并发请求回 `extra.error="another image download is in progress"`。
壁纸为会话级，重启后恢复纯色背景。

### 4.2 `notify` 详解

```json
{
  "type": "notify",
  "audio_url": "https://cdn/notice.ogg",
  "id": "broadcast-12345",
  "subtitles": [{"start_ms": 0, "text": "您好"}, {"start_ms": 1500, "text": "这是一条通知"}]
}
```

- `audio_url` 必须是**不跳转的直链**，Opus 编码 OGG，建议同网段或公网 CDN
- 回执 `success=true` 只表示**已接受并开始播放**，不承诺音频成功；最终结果以
  `notify_result` 为准
- `id` 原样回传到 `reply` 与 `notify_result`，用于服务端对账，建议 ≤64 字符

### 4.3 `set_status` 详解

```json
{"type":"set_status","text":"这是一条较长的滚动文字","color":"red"}
{"type":"set_status","text":"橙色文字","color":"#FFA500"}
```

`color` 支持颜色名 `red` `green` `blue` `yellow` `orange` `white` `black` `cyan`
`purple` `gray`/`grey`，或 `#RRGGBB`。非法值回 `success=false`。
颜色只作用于顶部状态行，下一次状态变化或空闲时的时钟刷新会恢复主题色。

### 4.4 `query_status` 响应

回执的 `extra` 对象（**有值才携带**，无对应硬件时省略）：

| 字段 | 类型 | 说明 |
|---|---|---|
| `device_state` | string | 同 §3.1 |
| `battery` / `charging` | int / bool | 电量百分比、是否在充电 |
| `temperature` | float | 板载温度 |
| `ip` / `ssid` / `rssi` | string / string / int | 网络信息（蜂窝板型省略） |
| `theme` | string | 当前主题名 |
| `volume` / `brightness` | int | 音量、背光百分比 |
| `preview_active` / `wallpaper_active` | bool | 是否有预览图 / 壁纸 |
| `imu_enabled` | bool | IMU 开关（无 IMU 板型省略） |
| `wake_word_running` | bool | 唤醒词检测是否运行 |
| `notify_active` / `notify_id` | bool / string | 播报进行中及其 id |
| `mqtt_connected` | bool | 控制通道连接状态 |

```json
{
  "type": "reply", "command_type": "query_status", "success": true, "device_state": "idle",
  "extra": {
    "device_state": "idle", "battery": 87, "charging": false, "temperature": 41.5,
    "ip": "192.168.6.55", "ssid": "Temi", "rssi": -59, "theme": "light",
    "volume": 70, "brightness": 80, "preview_active": false, "wallpaper_active": false,
    "imu_enabled": true, "wake_word_running": true, "notify_active": false,
    "mqtt_connected": true
  }
}
```

---

## 5. `extra.error` 错误码

出现 `success=false` 时，`extra.error` 给出具体原因：

| 指令 | error 取值 |
|---|---|
| `set_image` | `another image download is in progress`、`no LVGL display`、`no network`、`http client unavailable`、`http open failed`、`http status is not 200`、`empty body`、`download incomplete`、`download failed`、`image too large (max 4 MB)`、`image too large (max 1280x1280, 1 MP)`、`jpeg decode failed`、`out of memory`、`failed to apply background image` |
| `set_theme` | `missing theme name`、`display does not support themes`、`unknown theme (supported: light, dark)` |
| `set_status` | `unknown color (use red/green/blue/yellow/orange/white/black or #RRGGBB)` |
| `set_imu` | `IMU not available on this board` |

其余指令（`listen` / `abort` / `mcp` / `system` / `set_volume` / `set_brightness` /
`toggle_wake_word` / `notify` / `abort_notify` / `toggle_power_save`）失败时只回
`success=false`，不带 `extra`——通常是状态约束或硬件能力不满足。

---

## 6. 连接周期

| 事件 | 表现 |
|---|---|
| 首次连接 | 独立任务执行阻塞式 `Connect()`（≤10 s），不阻塞主循环 |
| 连接成功 | 自动 `Subscribe(cmd)` + 发 `hello` |
| 连接失败 | **不销毁客户端**，交给 esp-mqtt 内置 auto-reconnect 持续重试 |
| 网络抖动 / 断网 | **不拆除通道**，重连后自动重新订阅并重发 `hello` |
| 设备重启 | 重新 `hello`；`hello.status` 为重启后的当前状态 |
| 主动停止 | 先发 `goodbye`，等待 50 ms 冲刷发送队列后断开 |

重连时 `hello` 会清空播报簿记，因此断连期间结束的播报**不会补发** `notify_result`。

服务端判断在线：无需设备端 LWT。订阅 broker 系统事件即可——
EMQX 为 `$events/client_connected` / `$events/client_disconnected`，
Mosquitto 2.0+ 为 `$SYS/broker/connection/{clientid}/state`。

---

## 7. 典型对话示例

**远程播报并确认结果**

```
→ {"type":"notify","audio_url":"https://cdn/a.ogg","id":"n-1"}
← {"type":"reply","command_type":"notify","success":true,"device_state":"notifying","id":"n-1"}
← {"type":"session_update","device_id":"...","status":"notifying"}
← {"type":"notify_result","device_id":"...","id":"n-1","outcome":"finished","duration_ms":5416}
← {"type":"session_update","device_id":"...","status":"idle"}
```

**下发图片并确认显示**

```
→ {"type":"set_image","url":"https://cdn/c.jpg"}
← {"type":"reply","command_type":"set_image","success":true,"device_state":"idle"}
→ {"type":"query_status"}
← {"type":"reply","command_type":"query_status","success":true,"device_state":"idle",
   "extra":{...,"preview_active":true,"wallpaper_active":false}}
```

---

## 8. 调试工具

| 脚本 | 用途 |
|---|---|
| `scripts/control_mqtt_monitor.py` | 订阅 reply topic，实时打印设备上行消息 |
| `scripts/notify_probe.py` | 播报闭环测试，`--case` 支持 `query` / `notify` / `abort` / `abort-idle` |

```sh
/Users/tong/.workbuddy/binaries/python/envs/default/bin/python \
    scripts/notify_probe.py --device <UUID> --audio-url <直链> --case notify
```
