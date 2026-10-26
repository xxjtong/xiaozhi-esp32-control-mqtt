# 远程语音播报服务设计评估

方案二：**服务端合成语音，设备下载播放**（对应 `notify` 指令）。

本文是设计评估，不是实施记录。所有结论均基于当前仓库代码与 `docs/control-mqtt.md`、
`docs/notify.md` 的实际实现。

---

## 1. 结论摘要

| 项 | 结论 |
|---|---|
| 固件改动 | **基础播放能力零改动**（`notify` 与 `NotifyPlayer` 已实现并真机验证）；**送达闭环与本地停止手势为后续增量**，见下方「1.1 实施状态」 |
| 服务端工作量 | 约 3–4 人日（含 TTS 接入、编码封装、下发器、状态机）；有了 `notify_result` 后 M4 的状态推断可大幅简化 |
| 主要工程量 | 不在设备端，而在**服务端 TTS 合成与音频托管** |
| 最大风险 | ~~`notify` 无送达确认~~（已通过 `notify_result` 解决）；离线仍不缓存，需服务端自己排队重发 |
| 建议策略 | **预生成音频 + 直链下发**，规避 5 秒超时与播放欠载 |

方案成立的前提已被代码证实：`NotifyPlayer` 的 HTTP 增量下载 + Ogg 解复用 + 共享解码队列
链路已经跑通，服务端只需提供符合规格的音频 URL。

### 1.1 实施状态（2026-10-26 更新）

本文写成后，设备侧已完成闭环增强并通过真机验证，因此「零改动」仅对**基础播放**成立：

| 增量 | 说明 | 文档 |
|---|---|---|
| `notify` 回执语义修复 + 可选 `id` | 原先无条件回 `success=true`；现复查是否真的进入 notifying | [control-mqtt.md](control-mqtt.md) |
| 新增 `notify_result` 上行消息 | `outcome`（`finished`/`cancelled`/`user_stopped`/`aborted`/`interrupted`）+ `duration_ms`，服务端可直接对账，不必靠 `duration_ms` 启发式推断 | [control-mqtt.md](control-mqtt.md) §5.3 |
| 新增 `abort_notify` 下行指令 | 远程取消正在播放的播报 | [control-mqtt.md](control-mqtt.md) |
| 本地手势停止播报 | 播报中屏幕双击 / BOOT 键 / IMU 姿态变化均可停止，上报 `event notify_stopped` 与 `outcome=user_stopped` | [control-mqtt-notify-feedback.md](control-mqtt-notify-feedback.md) |

服务端因此**不再需要**本文 §6 里"靠 `duration_ms` 猜有没有播完"的启发式逻辑，直接读 `notify_result`
即可。这把 M4 的工作量从"状态推断 + 重试"降为"订阅回执 + 超时兜底"。

---

## 2. 方案边界

**做什么**

- 服务平台向指定设备推送一段语音播报（TTS 合成），可选同步显示字幕
- 支持按设备、按群组的定向与广播
- 提供播报任务的接受/完成推断与失败重试

**不做什么**

- 不建立双向对话。设备播报期间不开启麦克风上行，播完自动回 Idle
- 不替代官方 ASR+LLM+TTS 对话链路。需要"用户说话 → 设备回答"时用 `listen start`
- 不做实时流式对讲。那种场景应走官方 UDP 通道，不在本方案范围

---

## 3. 总体架构

```
┌──────────────────────────── 服务端 ────────────────────────────┐
│                                                                │
│  播报 API ──▶ 任务队列 ──▶ TTS 合成 ──▶ Opus 编码 ──▶ Ogg 封装  │
│                    │                                    │       │
│                    │                                    ▼       │
│              去重/缓存（文本 hash）              对象存储 / 静态服务 │
│                    │                                    │       │
│                    ▼                                    │       │
│            设备注册表（在线/状态）                         │       │
│                    │                                    │       │
│                    ▼                                    ▼       │
│              MQTT 下发器 ────── notify(audio_url) ─────────     │
└────────────────────────────────┬───────────────────────────────┘
                                 │ MQTT（xiaozhi/<uuid>/cmd）
                                 ▼
                          ┌──────────────┐      HTTP GET（直链）
                          │  ESP32 设备   │ ─────────────────────▶ 音频
                          │  Idle 播报    │ ◀─────────────────────
                          └──────────────┘
                                 │ MQTT（xiaozhi/<uuid>/reply）
                                 ▼
                           reply / session_update / event
```

设备侧内部链路（已实现，无需改动）：

```
ControlMqtt::HandleNotify
  └─▶ Application::StartNotification      // 仅 Idle 接受
        └─▶ NotifyPlayer::Start           // 独立任务 notify_http
              ├─ HTTP GET 1KB 循环读（Accept: audio/ogg）
              ├─ OggDemuxer 逐包解出 Opus
              └─▶ audio_service_.PushPacketToDecodeQueue
                    └─▶ 共享 Opus 解码队列（20 包 ≈ 1.2s）→ 播放
```

---

## 4. 音频规格（硬约束）

设备端 `NotifyPlayer` + `OggDemuxer` 对音频有明确限制，违反会静默失败（只在串口日志报错）。

| 项 | 要求 | 违反后果 |
|---|---|---|
| 容器 | Ogg（Ogg Opus） | 解复用失败，播放中止 |
| 编码 | Opus，单流 | 同上 |
| 声道 | **单声道** | 立体声被明确拒绝 |
| 采样率 | 16 kHz 或 24 kHz | 其它值需设备重采样，建议直接用这两种 |
| 帧长 | 5–120 ms，建议 **60 ms** | 2.5 ms 包被拒绝；60 ms 与固件 `OPUS_FRAME_DURATION_MS` 一致 |
| 码率 | 建议 24–32 kbps | 语音场景足够，文件更小 |
| 传输 | HTTP/HTTPS，2xx | 非 2xx 直接失败 |
| 编码方式 | `Content-Length` 或 chunked 均可 | — |
| 重定向 | **不跟随** | 302 会导致下载失败 |
| 单条时长 | 建议 **≤5 秒** | 见下方超时说明 |

生成示例：

```sh
ffmpeg -i input.wav -c:a libopus -b:a 24k -ar 16000 -ac 1 \
       -frame_duration 60 output.ogg

ffprobe -v error -show_entries stream=codec_name,sample_rate,channels \
        -of default=nw=1 output.ogg
# 期望：codec_name=opus / sample_rate=16000 / channels=1
```

体积参考：16 kHz / 24 kbps 约 3 KB/s，5 秒语音约 15 KB。

---

## 5. 关键设计决策

### 5.1 预生成优先，而非实时流式

虽然 notify 支持 chunked 流式传输，但有两个硬约束倾向于预生成：

1. `notify_player.cc` 中 `kHttpTimeoutMs = 5000`；TTS 合成常常超过这个量级
2. 播放缓冲仅 20 包（约 1.2 秒），合成速度跟不上会触发 `Notification playback underrun`

因此推荐流程：**接收播报请求 → 异步合成 → 落盘/对象存储 → 下发 URL**。
同步 API 应返回任务 ID，合成完成后再下发。这样首包延迟完全由 TTS 决定，与传输无关。

### 5.2 URL 必须是最终直链

设备不跟随重定向。若使用对象存储或 CDN，必须下发**最终可访问的直链**，
不能用短链、跳转页或需要鉴权重定向的地址。URL 有效期应大于"下发到播放完成"的时间窗。

### 5.3 文本去重缓存

以 `hash(文本 + 音色 + 语速 + 采样率)` 为 key 缓存音频文件。播报场景中
固定话术（"任务完成""设备离线"等）复用率很高，可显著降低 TTS 成本与延迟。

### 5.4 按设备串行下发

设备端 `notify_player_.IsBusy()` 时会忽略新播报，且非 Idle 状态直接回 `success=false`。
服务端必须维护每设备的播报队列，**串行发送、上一条结束后再发下一条**，
否则会出现大量静默丢弃。

---

## 6. 下发消息格式

订阅 topic：`xiaozhi/<设备完整UUID>/cmd`
回复 topic：`xiaozhi/<设备完整UUID>/reply`
（UUID 取自 `Board::GetInstance().GetUuid()`，client_id 为 `xiaozhi-<UUID后8位>`）

```json
{
  "type": "notify",
  "audio_url": "https://cdn.example.com/audio/xxxx.ogg",
  "subtitles": [
    { "start_ms": 0,    "text": "任务已完成" },
    { "start_ms": 1800, "text": "结果已保存" }
  ]
}
```

- `audio_url` 必填；`subtitles` 可选
- 设备按 `start_ms` 排序，播放位置跨过条目时更新屏幕文字
- 消息无通知 ID、无状态字段，服务端需自行生成 `broadcast_id` 做内部追踪

手工验证：

```sh
mosquitto_pub -h mqtt.virsical.cn -p 21883 -u admin -P public \
  -t "xiaozhi/<UUID>/cmd" \
  -m '{"type":"notify","audio_url":"http://192.168.1.10:8000/test.ogg","subtitles":[{"start_ms":0,"text":"测试播报"}]}'
```

---

## 7. 送达确认与状态推断

这是本方案最需要设计的部分，因为 `notify` **没有真正的 ACK**。

可用信号按可信度排序：

| 级别 | 信号 | 含义 | 可靠性 |
|---|---|---|---|
| L1 | `reply` topic 收到 `{"type":"reply","command_type":"notify","success":true}` | 设备接受了指令 | 中 |
| L2 | `session_update` 出现 `status: "notifying"` | 设备确实开始播报 | 高 |
| L3 | `session_update` 由 `notifying` 变回 `idle` | 播报正常结束 | 高 |

**注意 L1 的陷阱**：`ControlMqtt::HandleNotify` 校验 `audio_url` 非空且设备状态为 Idle 后，
会先调用 `StartNotification()` 再**无条件** `SendReply("notify", true)`。
若 `StartNotification()` 内部因忙碌而提前返回，reply 仍是 `success=true` 但实际没播。
所以**不能用 L1 作为播放成功的判据**，必须依赖 L2/L3。

失败判定建议：下发后 5 秒内未观测到 `status: notifying`，判定为失败并重试一次；
连续失败则标记设备异常。

在线判定：设备端未配置 MQTT LWT，服务端需按"最近一次上行消息时间"超时判定离线。
如需更可靠，可在设备端补一个 LWT（小改动），或依赖 broker 的连接事件。

---

## 8. 风险登记

| 风险 | 影响 | 缓解措施 |
|---|---|---|
| 非 Idle 时播报被拒 | 指令静默丢失 | 服务端维护状态表，仅在 idle 下发；其余入队等待 |
| 无 ACK、离线不缓存 | 无法保证送达 | L2/L3 状态推断 + 有限重试 + 失败告警 |
| 5 秒 HTTP 超时 | 长音频下载中断 | 预生成 + 单条 ≤5 秒 + 内网或 CDN 加速 |
| URL 重定向 | 下载直接失败 | 强制直链，接入前用 curl 验证无 302 |
| 播放欠载卡顿 | 听感断续 | 预生成；避免实时合成 |
| 唤醒词可打断播报 | 播报不完整 | 产品侧接受；关键播报可重试或改用 `set_status` 兜底 |
| 明文 MQTT 凭据暴露 | 安全风险 | 生产换 8883 + TLS，或仅走内网；Kconfig 默认凭据发布前清空 |
| 同一音频被重复合成 | 成本浪费 | 文本 hash 缓存 |

---

## 9. 部署与安全

- **音频服务**：与 MQTT broker 可同机部署（内网 PoC），生产建议独立静态服务或对象存储
- **协议**：开发可用 `http://`（局域网直连最省事），生产必须 `https://`
- **URL 鉴权**：若需防盗链，用带签名的直链（签名放 query，不触发重定向）
- **凭据**：`admin/public` 仅可用于内网验证；生产环境改由 NVS 注入并启用 TLS
- **广播**：多台设备时服务端并行下发，但**每台设备内部仍串行**

---

## 10. 验证计划

| 阶段 | 内容 | 通过标准 |
|---|---|---|
| P0 素材 | ffmpeg 生成 16k/单声道/60ms 的 ogg，本地 `python -m http.server` 托管 | ffprobe 参数正确，curl 无重定向 |
| P1 单条 | mosquitto_pub 下发 notify | 设备播报，串口日志无 error，reply 回 success=true |
| P2 状态 | 订阅 reply topic 观察 `session_update` | 出现 `notifying` → `idle` 完整序列 |
| P3 边界 | 非 Idle 时下发、错误 URL、立体声文件 | 分别复现 `success=false`、`status %d`、解复用失败 |
| P4 服务端 | 接入 TTS + 缓存 + 队列 | 文本到播报端到端闭环，重复文本命中缓存 |
| P5 压测 | 连续下发、离线设备、弱网 | 无静默丢弃，失败可观测 |

---

## 11. 工作量与里程碑

| 里程碑 | 内容 | 预估 |
|---|---|---|
| M1 | 音频生成与托管（ffmpeg + 静态服务），真机跑通单条播报 | 0.5 天 |
| M2 | TTS 接入 + Opus/Ogg 封装 + 文本缓存 | 1 天 |
| M3 | MQTT 下发器 + 设备注册表 + 串行队列 | 1 天 |
| M4 | 状态推断（L2/L3）、重试、失败告警 | 1 天 |
| M5 | 联调与压测 | 0.5–1 天 |

**基础播放**链路固件侧零改动，这是本方案相对"复用 UDP 通道"方案的最大优势。
送达闭环（`notify_result` / `abort_notify` / 本地停止手势）是 2026-10-26 追加的设备侧增量，
已实施并完成 10 项真机验证，详见
[control-mqtt-notify-feedback.md](control-mqtt-notify-feedback.md)；服务端可据此去掉状态推断逻辑。
