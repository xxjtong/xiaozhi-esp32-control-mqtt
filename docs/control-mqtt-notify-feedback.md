# 第二路 MQTT 改造方案：播报闭环反馈（设计稿）

本文只针对**第二路 MQTT 控制通道**的固件改造，服务于
[远程语音播报服务设计评估](./tts-broadcast-design.md)（方案二）。
目标是让服务端能可靠判断一次 `notify` 播报的接受、结束与中断。

状态：**已实施并编译通过**（ESP-IDF v6.0.2，改动仅 `main/control_mqtt.h` / `main/control_mqtt.cc`）。

```
xiaozhi.bin binary size 0x2efbe0 bytes. Smallest app partition is 0x3f0000 bytes. 0x100420 bytes (25%) free.
```

无 error、无 warning；相对改动前增大 3264 字节。真机回归尚未执行。

---

## 1. 范围界定

### 只改这两个文件

- `main/control_mqtt.h`
- `main/control_mqtt.cc`

### 明确不改

| 不改 | 原因 |
|---|---|
| `main/notify/`（`NotifyPlayer`） | 播放器逻辑已稳定且真机验证过 |
| `main/protocols/` | 官方协议通道零侵入是既有约束 |
| `main/application.cc` 状态机 | 避免触及核心行为 |
| `Mqtt` 抽象接口与各实现 | 影响 WiFi/ML307 两条实现链 |
| `main/Kconfig.projbuild` | 本次不新增配置项 |

### 可行性已核实

- `Application::StartNotification()` 与 `Application::StopNotification()` 均为 **public**
  （`main/application.h` 127 / 181 行），`GetDeviceState()` 也是 public
- `ControlMqtt::BroadcastState(state_name)` 由主任务在**每次状态变化时**调用，
  因此无需改 `application.cc` 即可观测 `idle → notifying → idle` 全过程
- 这两点使本方案能完全收敛在 `control_mqtt.*` 内

---

## 2. 现状缺口

| # | 缺口 | 后果 |
|---|---|---|
| 1 | `HandleNotify` 在 `StartNotification()` 后**无条件**回 `success=true` | 服务端把"没播"当成"播了"，告警与统计全失效 |
| 2 | notify 无通知 ID | 服务端无法把 reply / 状态变化与某次播报对账 |
| 3 | 播报是"开环"的 | 结束、失败、被唤醒打断，服务端全部不可见 |

---

## 3. 改动清单

### 3.1 修复 notify 回执语义

`StartNotification()` 内部会同步调用 `SetDeviceState(kDeviceStateNotifying)`；
若因忙碌或非 Idle 提前返回，状态不会变成 `notifying`。因此只需在调用后复查一次状态：

```cpp
// ControlMqtt::HandleNotify，替换原有的无条件 SendReply("notify", true)
Application::GetInstance().StartNotification(std::move(audio_url), std::move(subtitles));
const bool accepted =
    Application::GetInstance().GetDeviceState() == kDeviceStateNotifying;
SendReply("notify", accepted, "", /* extra */ nullptr);
```

零侵入、无需新接口。`accepted` 的语义是**"设备已接受并开始播报"**，
不承诺音频一定播放成功（URL 404、解复用失败仍会走到这里）。

### 3.2 支持 broadcast_id 回传

- 下行 `notify` 增加可选 `id`（字符串，服务端生成，建议 ≤64 字符）
- `SendReply()` 增加可选 id 参数，回执中回传 `id` 字段
- 未携带 `id` 时行为与现在完全一致（向后兼容）

### 3.3 新增上行消息 `notify_result`

在 `BroadcastState()` 内维护播报生命周期状态机（新增成员 `last_state_`、
`notify_id_`、`notify_start_ms_`）：

| 观测到的迁移 | 上报 `outcome` | 含义 |
|---|---|---|
| `idle → notifying` | （仅记录起始） | 播报已开始 |
| `notifying → idle` | `finished` | 播放结束，或播放失败 |
| `notifying → idle`（且 `cancel_requested_` 为真） | `cancelled` | 被 `abort_notify` 主动取消 |
| `notifying → connecting` / `listening` | `aborted` | 被唤醒词打断 |
| `notifying → activating` / `upgrading` | `interrupted` | 被 OTA/激活流程抢占 |

`cancelled` 必须在 ControlMqtt 内部用一个 `cancel_requested_` 标志区分，
因为主动取消与自然结束的状态迁移完全相同（都是 `notifying → idle`），
仅靠状态无法分辨。该标志在 `abort_notify` 处理时置位，在发出
`notify_result` 后清零。

消息格式：

```json
{
  "type": "notify_result",
  "device_id": "0d0cef91-...",
  "id": "broadcast-12345",
  "outcome": "finished",
  "duration_ms": 3120
}
```

`duration_ms` 自进入 `notifying` 起算，供服务端判断是否为异常短的失败。

### 3.4 新增下行指令 `abort_notify`

```json
{ "type": "abort_notify" }
```

调用 public 的 `Application::StopNotification()`。仅当当前状态为 `notifying`
时回 `success=true`，否则 `false`。这让服务端能主动取消一条播报
（例如更高优先级的紧急播报要插队）。

**停止能力已核实可用**（见 3.6）。处理时需先置位 `cancel_requested_`，
再触发停止，以便随后上报 `outcome=cancelled` 而非 `finished`。

**实现偏差**：`Application::StopNotification()` 实际是 **private**
（只有 `StartNotification` 是 public，`application.h` 181 行 vs 127 行），
直接调用会编译失败。为遵守"只改第二路 MQTT"的约定，改用 public 且事件驱动的
`Application::StopListening()`：在 notifying 状态下其处理器
`HandleStopListeningEvent()` 只调用 `StopNotification()`，无其它分支副作用。

代价是停止动作在**下一个主循环迭代**才生效，因此 `abort_notify` 的回执中
`device_state` 仍是 `notifying`。服务端应以 `notify_result` 为准。

若将来希望回执状态也是最终态，可把 `StopNotification()` 声明移到 public
（项目已有先例：`StartNotification` 当初就是为了控制通道移到 public 的，见
`docs/control-mqtt.md` 阶段一）。

### 3.5 `query_status` 增加播报字段

在 `extra` 中增加 `notify_active`（bool）与 `notify_id`（播报中才有）。
便于服务端在下发前确认设备空闲，也便于脚本断言。

### 3.6 停止播放能力核实（结论：已具备，无需新增）

`abort_notify` 依赖的 `Application::StopNotification()` 是**现成的成熟路径**，
已核实其停止链路完整：

```cpp
void Application::StopNotification() {
    notify_player_.Stop();          // 置 cancelled_，HTTP 任务停止解包与入队
    audio_service_.ResetDecoder();  // 清空解码队列 + 播放队列，重置 Opus 解码器
    board.GetDisplay()->SetChatMessage("assistant", "");
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (GetDeviceState() == kDeviceStateNotifying) SetDeviceState(kDeviceStateIdle);
}
```

`AudioService::ResetDecoder()`（`audio_service.cc` 776 行）会：
`++playback_generation_`（让已入队的旧包失效）、`esp_opus_dec_reset()`、
清空 `audio_decode_queue_`、`audio_playback_queue_`、`timestamp_queue_`。
**已经入队但尚未播出的音频会被直接丢弃**，这正是取消播报想要的行为。

该方法在 `application.cc` 中已有 8 处调用（网络断开、错误、唤醒词、
开始/停止聆听、重启、OTA、重置协议），属经过真机验证的路径。
因此 `abort_notify` 只是把它暴露为一条 MQTT 指令，**无新增风险**。

时序注意：`notify_player_.Stop()` 仅置取消标志，HTTP 任务可能仍在阻塞读取，
最长残留至 `kHttpTimeoutMs`（5 秒）；但期间不会再向解码队列推包，故听感上已停止。

---

## 4. 消息格式变更汇总

**下行（cmd topic）**

| 指令 | 变更 |
|---|---|
| `notify` | 新增可选 `id`；回执 `success` 语义修正为"已接受并开始播报"，回执回传 `id` |
| `abort_notify` | **新增** |

**上行（reply topic）**

| 消息 | 变更 |
|---|---|
| `reply` | 新增可选 `id` 字段 |
| `notify_result` | **新增** |

---

## 5. 在线判定：不改固件也能做

原设想是给设备加 MQTT LWT，但核实后发现**不需要**：

- `Mqtt` 抽象接口（`Connect` 签名）不支持 will 消息，加 LWT 要改接口与
  WiFi/ML307 两套实现，超出本次范围
- 设备端 `client_id` 是确定的 `xiaozhi-<UUID后8位>`，服务端可直接查 broker

推荐做法（**纯服务端**）：订阅 broker 的系统事件或调用管理 API。

- EMQX：`$events/client_connected`、`$events/client_disconnected`，或
  `GET /api/v5/clients/{clientid}`
- Mosquitto 2.0+：`$SYS/broker/connection/{clientid}/state`

这样在线判定精确，且设备端零改动、零额外流量。

---

## 6. 已知边界（如实说明）

| 边界 | 说明 | 可选突破 |
|---|---|---|
| `finished` 不区分"播完"与"播放失败" | `HandleNotificationFinished(success)` 的结果在 `application.cc` 内部，ControlMqtt 取不到 | 启发式：`duration_ms` 明显短于预期（弹窗音约 0.5–1 s，建议阈值 800 ms）判为疑似失败 |
| 精确的成功/失败三态 | 需在 `application.cc` 暴露一行回调（约 3 行） | 若 P5 压测证明确实需要，作为独立小改动提出 |
| `notify_result` 为 QoS 0 尽力而为 | broker 离线期间丢失不补发 | 服务端结合在线状态与超时兜底 |
| 断连期间的簿记可能陈旧 | 见下方说明 | 每次（重）连 `SendHello()` 清空簿记 |

`notify_result` 与既有 `event` 消息保持同一约定：QoS 0、离线丢弃、
只携带元数据（不含音频内容）。

### 6.1 断连导致的陈旧簿记（已处理）

`application.cc` 只在链路在线时才调用 `BroadcastState()`：

```cpp
if (control_mqtt_ && control_mqtt_->IsConnected()) {
    control_mqtt_->BroadcastState(DeviceStateToString(new_state));
}
```

因此若播报在断连期间结束，`last_state_` 会停留在 `notifying`；重连后下一次**无关的**
状态变化（如用户唤醒 `idle → connecting`）会被误判成播报被中断，上报一条
`outcome=aborted` 且 `duration_ms` 异常的假消息。

处理：`SendHello()` 开头清空 `last_state_`、`notify_id_`、`notify_start_ms_`、
`notify_active_`、`cancel_requested_`。每次连接与重连都会重置，杜绝误报。
代价是断连期间结束的那次播报不会补发 `notify_result`——这与"离线丢弃"的既有约定一致。

若要彻底解决（让离线期间的状态变化也被跟踪），可去掉上面那个 `IsConnected()`
判断：`SendSessionUpdate()` 内部已有 `connected_` 检查，离线时同样不会发消息。
但该改动会触及 `application.cc`，故本方案未采用。

---

## 7. 验证计划

| 用例 | 操作 | 期望 |
|---|---|---|
| 正常播报 | 下发带 `id` 的 notify | reply `success=true` 且回传 `id`；随后 `notify_result outcome=finished` |
| 非 Idle 下发 | 设备在 speaking 时下发 | reply `success=false`，无 `notify_result` |
| 坏 URL | 下发指向 404 的 URL | reply `success=true`（已接受）；`notify_result outcome=finished` 且 `duration_ms` 偏短 |
| 唤醒打断 | 播报中说唤醒词 | `notify_result outcome=aborted` |
| 主动取消 | 播报中下发 `abort_notify` | reply `success=true`，声音立即停止，`notify_result outcome=cancelled` |
| 空闲时取消 | 非播报状态下发 `abort_notify` | reply `success=false`，不产生 `notify_result` |
| 无 id 兼容 | 下发不带 `id` | 行为与改动前一致 |
| 编译 | `idf.py build` | 通过，无新增 warning（`-Wall -Wextra -Werror`） |
| 格式 | `clang-format --dry-run -Werror main/control_mqtt.{h,cc}` | 通过 |

改动落地后需同步更新 `docs/control-mqtt.md`（新增"阶段十五"）与本文状态。

---

### 7.1 真机实测记录（2026-10-26）

设备 `0d0cef91-...-1e2d364c`，固件 2.4.2.1，板型 `waveshare/esp32-s3-touch-lcd-3.49-v2`。
测试脚本见 §7.2。

| # | 用例 | 结果 |
|---|---|---|
| 1 | `query_status` | ✅ `extra` 中返回 `notify_active: false`，新字段生效 |
| 2 | 正常播报 | ✅ `Notification playback 7 completed`；`reply` 带回 `id=test-001`、`device_state=notifying`；`notify_result outcome=finished duration=5416 ms`（4.9 s 音频 + 弹窗音） |
| 3 | 播报中 `abort_notify` | ✅ 状态同毫秒 `notifying → idle`；`notify_result outcome=cancelled duration=2161 ms` |
| 4 | 空闲时 `abort_notify` | ✅ `success=false`（正确拒绝，不产生 `notify_result`） |
| 5 | 唤醒打断 | ✅ 播报中说唤醒词「你好，小威」→ `event wake_word`（state=notifying）+ `notify_result outcome=aborted duration=15424 ms`，随后 connecting → listening → speaking |
| 6 | 失败场景 | ✅ 网络失败时同样上报 `notify_result`（`duration=5080/283 ms`） |
| 7 | 播报中屏幕双击 | ✅ `event notify_stopped=double_click`（state=notifying）+ `notify_result outcome=user_stopped duration=2313 ms`（37 s 音频 2.3 s 掐断）；无 `theme_changed`、无 `motion_detected` |
| 8 | IMU 姿态触发聆听 | ✅ 空闲晃动进聆听（现场确认屏幕显示"聆听中"）；播报中晃动同样生效：`motion_detected`（state=notifying）→ `outcome=user_stopped` → idle → listening |
| 9 | 喇叭振动是否误触发 IMU | ✅ 播报全程不触碰设备，`motion_detected` 0 次，`outcome=finished duration=43692 ms` —— 本机喇叭振动不足以越过 0.1 g 阈值 |
| 10 | 空闲时双击清预览图（回归） | ✅ `Clearing persistent preview image on double-click` + `event image_cleared=double_click`，`preview_active` 由 true 变 false —— 新增的"播报中双击停播报"分支未破坏原有清图行为 |

用例 10 的排障过程值得一提：`set_image` 回 `success=true`、`preview_active` 也为 true，
但串口 50 s 内一条点击日志都没有，一度怀疑预览容器吞掉了点击。实际原因是**下发与现场操作
之间隔太久**，而 `preview_active` 反映的是 `HasPreviewImage()`（缓存是否为空），
即使图片已被 `PREVIEW_IMAGE_DURATION_MS`（5 s）自动隐藏它也仍为 true —— 不能拿它判断"是否可见"。
排查点击类手势时直接看串口 `CustomLcdDisplay` 日志最快。

### 7.1.1 复查发现并修复的问题（第二轮）

功能跑通后做了一轮交叉复查，修掉 4 处：

| # | 问题 | 修复 |
|---|---|---|
| 1 | IMU 由 `ToggleChatState()` 改 `StartListening()` 后，聆听模式被硬编码成 `kListeningModeManualStop`，AEC 关闭时**说完话不会自动停止收音**（原默认模式是 AutoStop） | `Application` 新增 `StartListening(ListeningMode)` 重载 + `pending_listening_mode_`；`GetDefaultListeningMode()` 提到 public；IMU 改用设备默认模式 |
| 2 | 播报中按 BOOT 键 / 远程下发 `listen` 都会停播报，但没打标记 → `notify_result` 误报 `finished` | `HandleToggleChatEvent()` 与 `HandleStartListeningEvent()` 的 notifying 分支改调 `StopNotificationByUser()`；`HandleListen()` 在播报中先置 `cancel_requested_`，并把 outcome 判定顺序改为**远程优先于本地** |
| 3 | 抑制窗（700 ms）到期后的第一拍，基线仍是抖动期的采样值，重击余振会在窗后补触发一次 | 新增 `tap_settle_pending_`，窗后第一拍丢弃 |
| 4 | `notify_stopped` 事件在 LVGL 任务上报，携带的 state 可能与实际执行时不一致 | 事件上报移入 `Schedule()` 内主线程执行，并加 notifying 复核 |

复查中排除的两个疑似问题（无需改）：

- `preview_viewport_` 与 `container_` 各挂了一份点击回调，怀疑一次点击触发两次导致单击被判成双击 —— 查证 `preview_viewport_ = lv_obj_create(screen)`（`lcd_display.cc:880`）与 `container_` 是**兄弟节点**，`EnableEventBubbling(container_)` 不会波及它，也不存在冒泡，故不会重复计数。
- 播报时喇叭自身振动可能越过 IMU 的 0.1 g 阈值导致播报被自己打断 —— 实测用例 9，未复现。

****用例 5 的关键前置**：唤醒词是**「你好，小威」**（`CONFIG_CUSTOM_WAKE_WORD="ni hao xiao wei"`、
`CONFIG_CUSTOM_WAKE_WORD_DISPLAY="你好，小威"`），不是默认的「你好小智」。首次测试失败就是因为
说错了唤醒词。播报期间 `wake_word_running` 实测为 `true`（AFE 引擎 + MultiNet 自定义唤醒词，
`IsAfeWakeWord()` 为真，故 `StartNotification()` 里的 `EnableWakeWordDetection()` 传的是 true）。

**`outcome=aborted` 为何可达**：`HandleWakeWordDetectedEvent()` 先 `StopNotification()`（→ idle）
再 `BeginWakeWordInvoke()`（→ connecting），两次 `SetDeviceState()` 在同一主线程调用中合并，
主循环只观察到最终的 `connecting`，因此判定为 `aborted` 而非 `finished`。

用例 7 顺带验证了点击抖动抑制**：双击屏幕时机身必然抖动，但整个窗口内没有
`motion_detected` 事件，说明 `ImuMonitor::NotifyUserInteraction()` 的 700 ms 抑制窗生效；
同时没有 `theme_changed`，说明播报中双击不会连带触发三击切主题、也不会清预览图。

**用例 6 印证了 §6 的边界**：播放失败与正常播完的 `outcome` 都是 `finished`，
实测失败样本的 `duration_ms` 为 5080（超时）与 283（内容无效），
与正常播完的 5416 差异明显——服务端只能用 `duration_ms` 做启发式判定。

**网络实测**：设备（192.168.6.x）拉取本机（192.168.41.x）HTTP 服务可行，
但路径 RTT 约 286 ms（隧道），**出现过一次首包超时**；12 KB 以内响应稳定。
生产环境务必把音频放在同网段或公网 CDN，不要用跨网段的临时服务。

### 7.2 测试脚本

`scripts/notify_probe.py`（paho-mqtt + pyserial）覆盖上面的用例，用法：

```sh
/Users/tong/.workbuddy/binaries/python/envs/default/bin/python \
    scripts/notify_probe.py --device <UUID> --audio-url <直链> --case <name>
```

`--case` 可选 `query` / `notify` / `abort` / `abort-idle`。

音频素材可用 macOS 自带 TTS 生成（模拟服务端合成）：

```sh
say -v Ting-Ting -o /tmp/test.aiff "您好，这是一条远程播报测试"
ffmpeg -y -i /tmp/test.aiff -c:a libopus -b:a 24k -ar 16000 -ac 1 \
       -frame_duration 60 /tmp/serve/test.ogg
```

---

## 8. 工作量

| 项 | 预估 |
|---|---|
| 3.1 回执语义修复 | 0.5 小时 |
| 3.2 broadcast_id | 1 小时 |
| 3.3 notify_result 状态机 | 3 小时 |
| 3.4 abort_notify | 1 小时 |
| 3.5 query_status 字段 | 0.5 小时 |
| 编译 + 真机回归 + 文档 | 3 小时 |
| **合计** | **约 1 人日** |

相对服务端 3–4 人日，固件侧投入很小，且完全不动核心模块。
