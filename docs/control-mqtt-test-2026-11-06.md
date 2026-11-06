# 控制通道 API 测试报告（2026-11-06）

> 被测设备：Waveshare ESP32-S3-Touch-LCD-4B
> 固件：`PROJECT_VER = 2.5.2.1`（2026-11-06 09:28 构建，ESP-IDF v6.1）
> 设备 UUID：`<device-uuid>`
> 设备 IP / WiFi：`192.168.x.x` / `<ssid>`
> 控制 broker：`mqtt.virsical.cn:21883`（`admin` / `public`）
> 测试时间：2026-11-06 11:03–11:13（Asia/Shanghai）

## 1. 结论摘要

> 2026-11-06 已针对下述 `set_image` 任务栈与 `notify` 超时做修复，复测结果见 §8。

| 类别 | 结果 |
|---|---|
| 下行指令 | 15 条中 **12 条完整通过**，2 条存在阻断性问题，1 条仅能验证转发 |
| 上行消息 | 6 类中 `hello`/`reply`/`session_update`/`notify_result` 完整通过；`event` 仅验证 1/7；`goodbye` 无法远程触发 |
| 错误与状态约束 | 主要错误路径均符合预期，状态不满足时返回 `success=false` |
| 设备状态 | 测试后已恢复：`idle`、音量 70、亮度 100、主题 light、唤醒词/IMU 开启、无预览图/壁纸 |

两个需要修复的问题：

1. **`set_image` 在 4B 上无法创建图片下载任务**，预览与壁纸均不可用；`clear:true` 正常。
2. **`notify` 的控制语义正常，但实际拉流播放失败**：TLS 握手约 9–11 s，随后 HTTP 头超时 5 s，`notify_result` 仍报 `finished`，实际没有出声。

## 2. 测试方法

使用独立 Python/paho 客户端订阅：

```
xiaozhi/<device-uuid>/reply
```

向对应 `cmd` topic 逐条下发指令，校验 `reply.command_type`、`success`、`device_state`、
`extra`，并同时采集 `session_update`、`notify_result`、`event`。设备串口日志用于交叉验证
任务创建、HTTP/TLS、状态机与播报结果。

测试脚本与原始数据：

| 文件 | 说明 |
|---|---|
| `tmp/control_api_test.py` | 自动化用例 |
| `tmp/control_api_test_results.json` | 用例级结果 |
| `tmp/control_api_test_messages.jsonl` | 全部 MQTT 收发原始报文 |
| `tmp/serial_control_test.log` | 测试期间串口日志 |

## 3. 下行指令结果

| # | 指令 | 结果 | 验证点 |
|---|---|---|---|
| 1 | `query_status` | PASS | `device_state/ip/ssid/rssi/theme/volume/brightness/preview_active/wallpaper_active/imu_enabled/wake_word_running/notify_active/mqtt_connected` 均返回；`battery`/`temperature` 按板级能力出现（本次部分查询带 `battery=0`，未带 `temperature`） |
| 2 | `set_volume` | PASS | 30 生效；150 被钳位为 100；恢复 70 |
| 3 | `set_brightness` | PASS | 40 生效；恢复 100 |
| 4 | `toggle_wake_word` | PASS | 关闭后 `wake_word_running=false`，重新开启为 `true` |
| 5 | `set_imu` | PASS | 关闭后 `imu_enabled=false`，重新开启为 `true` |
| 6 | `set_theme` | PASS | `dark`/`light` 均生效；非法主题返回 `unknown theme` |
| 7 | `set_status` | PASS（协议） | 十六进制色与颜色名均成功；非法颜色返回 `unknown color`；屏幕视觉效果未拍照验证 |
| 8 | `toggle_power_save` | PASS | `enable:true/false` 均成功；非 Idle 时正确拒绝 |
| 9 | `set_image` | **FAIL** | `preview` 与 `background` 均返回 `success=false`；串口：`Failed to create image download task`；`clear:true` 正常 |
| 10 | `notify` | **PARTIAL** | 接受、进入 `notifying`、回传 `id` 正常；实际音频拉流失败，未出声 |
| 11 | `abort_notify` | PASS | 播报中取消返回成功，`notify_result.outcome=cancelled`、`duration_ms≈2293`；Idle 时返回 `false` |
| 12 | `listen` | PASS | `start` 进入 `connecting→listening`；`stop` 回 `idle`；Idle 下 `stop` 幂等成功 |
| 13 | `abort` | PASS | Listening 下打断成功；Idle 下返回 `false` |
| 14 | `mcp` | **PARTIAL** | Idle 下回 `success=true`，Listening 下正确拒绝；该指令仅转发到主协议，控制通道看不到 MCP 结果 |
| 15 | `system` | PASS | `reboot` 回执后设备重启，约 10–12 s 后重新上线并发送 `hello`；未知子命令返回 `false` |

## 4. 上行消息与事件

| 消息/事件 | 结果 | 说明 |
|---|---|---|
| `hello` | PASS | 重启后收到，`device_id`、`status=idle`、`version=2.5.2.1`、`ip` 正确 |
| `reply` | PASS | 每条指令均有且仅有一次；确认不含 `device_id` |
| `session_update` | PASS | `idle→connecting→listening→speaking→idle`、`notifying→idle` 均观察到 |
| `notify_result` | PASS | 观察到 `finished` 与 `cancelled`，`id`、`duration_ms` 正确回传 |
| `event` | PARTIAL | 仅观察到 `interaction`（1/7）；其余 6 种需物理操作 |
| `goodbye` | 未验证 | 仅在 `ControlMqtt::Stop()` 时发送，无法通过远程指令触发 |

### 4.1 事件覆盖缺口

以下事件未能在本次自动化测试中触发，需要人工配合：

| 事件 | 触发方式 |
|---|---|
| `wake_word` | 对设备说“你好，邵怡” |
| `button_click` | 短按 BOOT 键 |
| `motion_detected` | 拿起/晃动设备 |
| `notify_stopped` | 播报中双击屏幕 |
| `image_cleared` | 有预览图时双击屏幕 |
| `theme_changed` | 三击屏幕 |

## 5. 错误与状态约束

以下用例均符合预期：

| 请求 | 预期 | 实测 |
|---|---|---|
| 未知 `type` | `success=false` | 通过 |
| `set_volume` 缺少 `level` | `false` | 通过 |
| `set_theme` 非法主题 | `false` + `extra.error` | 通过 |
| `set_status` 非法颜色 | `false` + `extra.error` | 通过 |
| `abort_notify` 在 Idle | `false` | 通过 |
| `abort` 在 Idle | `false` | 通过 |
| `notify` 缺少 `audio_url` | `false` | 通过 |
| `system` 未知子命令 | `false` | 通过 |
| `mcp` 在 Listening | `false` | 通过 |
| 播报中重复 `notify` | `false` | 通过 |
| 播报中 `toggle_power_save` | `false` | 通过 |
| `set_image clear:true` | `true` | 通过 |

## 6. 问题分析

### 6.1 `set_image`：图片下载任务创建失败

现象：

- 串口：`E (61901) ControlMqtt: Failed to create image download task`
- `reply`：`{"success":false}`，**没有 `extra.error`**
- 同时刻 `free sram` 约 19 KB；启动日志显示 AFE 初始化后最大连续块仅约 7.7 KB
- `clear:true` 不创建任务，始终成功

代码路径：`HandleSetImage()` 在通过能力检查后调用

```cpp
xTaskCreate(ImageFetchTaskEntry, "ctrl_image", 4096 * 3, args, 3, nullptr);
```

12 KB 栈需要从内部 RAM 分配连续块；4B 在 AFE/显示初始化后无法满足，因此 `xTaskCreate` 返回失败。
这是**4B 板级资源问题**，与图片 URL 无关；3.49 V2 早期验证可用的原因可能是其可用连续内存更大。

建议：

1. 将 `ctrl_image` 任务栈放到 PSRAM（`xTaskCreateWithCaps` 或静态 PSRAM 栈），与音频任务做法一致；
2. 或降低栈大小并实测 JPEG 解码路径；
3. 在 `xTaskCreate` 失败分支补充 `extra.error="failed to create image task"`，便于服务端定位。

### 6.2 `notify`：控制语义正常，实际播放失败

现象：

- `reply` 正常：`success=true`、`device_state=notifying`、`id` 原样返回
- `abort_notify` 正常：`outcome=cancelled`、`duration_ms` 约 2.3 s
- 但所有测试 URL（uguu、litterbox、loremfile、jsDelivr）均未出声：
  - 部分主机 TLS 握手直接失败；能握手的 uguu/jsDelivr 也耗时约 9–11 s
  - 随后 `HttpClient: Wait for HTTP headers receive timeout`（5 s）
  - `notify_result` 仍报 `finished`，`duration_ms` 为 14–16 s（对 1–2 s 音频明显异常）

日志示例：

```
I (89121) HttpClient: Established new connection to h.uguu.se:443 protocol=https cost=10810
E (90261) EspSsl: SSL receive failed: -132
E (94281) HttpClient: Wait for HTTP headers receive timeout
E (94281) NotifyPlayer: Notification HTTP status failed: Connection timed out (timeout)
```

结论：`notify` 的**状态机、回执、取消、结果上报均正常**；失败发生在 HTTP/TLS 拉流阶段，
因此本次无法确认 4B 的实际出声。`finished` 不区分成功与失败，服务端需结合 `duration_ms`
判断（与现有文档一致）。

建议：

1. 提高 `notify` 的 HTTP 头超时（当前 5 s），覆盖 TLS 握手 + 首包；
2. 或在 `NotifyPlayer` 中先完成 TLS/HTTP 预检，再进入 `notifying`；
3. 若 4B 内存紧张，检查 TLS 握手慢是否与堆碎片/PSRAM 配置有关；
4. 服务端对 `finished` 且 `duration_ms` 明显偏短的记录做失败告警。

### 6.3 `mcp` 只能验证转发

`ControlMqtt::HandleMcp()` 调用 `Application::SendMcpMessage()`，后者把 payload 发给主协议
（小智云），并不在本地调用 `McpServer::ParseMessage()`。因此控制通道只能确认：

- Idle 下 `mcp` 回 `success=true`；
- Listening 下正确回 `false`；
- 工具调用结果不会回到控制通道。

若需要“通过控制通道直接调用设备 MCP 工具并拿到结果”，需要额外实现本地 MCP 分发，
这不是当前固件的行为。

## 7. 复测建议

1. 修复 `set_image` 任务栈后，复测 preview/JPEG、background/PNG、clear 三种路径，并用 `query_status` 校验 `preview_active`/`wallpaper_active`。
2. 修复/调整 `notify` 超时后，用 1–2 s 与 10 s 的 Ogg Opus 各测一次，确认 `finished` 的 `duration_ms` 与音频时长匹配，并人工确认出声。
3. 人工触发 6 个本地事件，补全 `event` 覆盖。
4. 在设备正常停机/重启流程中验证 `goodbye`（例如主动关闭控制通道或进入 OTA 重启前）。
5. 在 3.49 V2 上复测 `set_image`，确认是否为 4B 独有的内存问题。

---

## 8. 2026-11-06 修复与复测

### 8.1 代码改动

| 文件 | 改动 |
|---|---|
| `main/control_mqtt.cc` | 图片下载任务改用 `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`；无 PSRAM 时回退 `xTaskCreate`；任务删除按栈来源选择 `vTaskDeleteWithCaps` / `vTaskDelete`；创建失败补 `extra.error` |
| `main/notify/notify_player.cc` | `kHttpTimeoutMs` 由 5 s 提高到 20 s，覆盖 TLS 握手 + 首包 |

构建：ESP-IDF v6.1，4B 变体，`xiaozhi.bin = 0x2db350`，分区余量 27%。
烧录：`idf.py app-flash`，保留 NVS/WiFi；设备重启后 `hello` 正常，版本仍为 `2.5.2.1`。
Host 测试：`python3 -m unittest discover -s scripts/tests -v` 全部通过。

### 8.2 `set_image` 复测

| 用例 | 结果 | 说明 |
|---|---|---|
| 640×480 PNG preview | **PASS** | 任务创建成功，`preview_active=true`，可 `clear` 清除 |
| 160×120 JPEG preview | **PASS** | 小图 JPEG 解码成功 |
| 240×180 JPEG preview | **PASS** | 小图 JPEG 解码成功 |
| 640×480 JPEG preview | **FAIL** | `extra.error="jpeg decode failed"`，串口 `Failed to allocate memory for JPEG output buffer` |
| 240×180 JPEG background | **FAIL** | `extra.error="failed to apply background image"`，`CreateCoverBackground` 需要 480×480 RGB565 缓冲（约 460 KB），当前 PSRAM 无法分配连续块 |
| `clear:true` | **PASS** | 清除预览与壁纸 |

结论：**任务栈问题已修复**，`set_image` 不再卡在 `Failed to create image download task`；预览图功能可用。
剩余问题是大图 JPEG 解码和壁纸 cover 缓冲的 PSRAM 连续内存不足，属于 4B 显示/PSRAM 布局限制，不是任务栈问题。

### 8.3 `notify` 复测

`kHttpTimeoutMs` 从 5 s 提高到 20 s 后：

| URL | 结果 |
|---|---|
| `cdn.jsdelivr.net` | TLS 握手约 9 s；能收到响应头，但读 body 时 0 字节断开（`Connection closed prematurely`） |
| `h.uguu.se` | TLS 握手约 9.6 s；等待响应头 20 s 超时 |
| `litter.catbox.moe` | TLS 握手直接失败（`-0x7280`） |
| `fastly.jsdelivr.net` | 约 42 s 后失败 |

`notify` 仍是 `success=true` → `notifying` → `notify_result finished`，但 `duration_ms` 为 12–45 s（对 1–2 s 音频明显异常），串口为 `Notification playback ... failed`，没有实际出声。

结论：**20 s 超时修复了“5 s 头超时必然失败”的问题，但无法解决该设备到外部 CDN 的 TLS/HTTP 拉流失败**。需要换用设备可达的音频源（同网段 HTTP 服务、企业内网 CDN 或已验证的 HTTPS 主机）再复测实际出声；仅调整超时不足以让当前公网 URL 播放成功。

### 8.4 复测建议（更新）

1. 在设备可达的同网段部署 HTTP 静态文件服务，验证 `notify` 的实际出声与 `duration_ms`。
2. 为 `set_image` 大图/壁纸预留启动期 PSRAM 缓冲，或改用按屏幕尺寸直接解码，避免额外 460 KB cover 缓冲。
3. 若必须支持公网 CDN，先在设备上验证目标 CDN 的 TLS 握手与首包时延，再决定超时值；20 s 会拉长失败场景的 `notifying` 停留时间。

---

## 9. 下发图片专项测试（2026-11-06 11:53–11:55）

> §9.1–§9.4 为调整 PSRAM 配置前的基线；针对 480×480 的最终配置与结果见 §9.5。

测试对象：修复后的 4B 固件（`0x2db350`，ESP-IDF v6.1）。
图片源：`http://via.placeholder.com/<尺寸>.<jpg|png>`（HTTP 直连，避免 TLS 干扰）。
每条用例后都下发 `{"type":"set_image","clear":true}`，并用 `query_status` 校验
`preview_active` / `wallpaper_active`。

### 9.1 结果矩阵

| 用例 | 期望 | 实测 reply | 状态标志 | 判定 |
|---|---|---|---|---|
| preview PNG 640×480 | 成功 | `success=true` | `preview_active=true` | **PASS** |
| preview PNG 320×240 | 成功 | `success=true` | `preview_active=true` | **PASS** |
| preview JPEG 160×120 | 成功 | `success=true` | `preview_active=true` | **PASS** |
| preview JPEG 240×180 | 成功 | `success=true` | `preview_active=true` | **PASS** |
| preview JPEG 320×240 | 成功 | `success=false`，`jpeg decode failed` | `preview_active=false` | **FAIL（PSRAM）** |
| preview JPEG 640×480 | 成功 | `success=false`，`jpeg decode failed` | `preview_active=false` | **FAIL（PSRAM）** |
| background PNG 640×480 | 成功 | `success=false`，`failed to apply background image` | `wallpaper_active=false` | **FAIL（PSRAM）** |
| background JPEG 240×180 | 成功 | `success=false`，`failed to apply background image` | `wallpaper_active=false` | **FAIL（PSRAM）** |
| oversized JPEG 1600×1200 | 尺寸超限拒绝 | `success=false`，`download incomplete` | 无变化 | 未完成（下载阶段超时） |
| 404 URL | `http status is not 200` | 符合 | 无变化 | **PASS** |
| 无效主机 | `http open failed` | 符合 | 无变化 | **PASS** |
| `clear:true` | 清除预览与壁纸 | `success=true` | 两者均为 `false` | **PASS** |

### 9.2 串口证据

```
I (13931) ControlMqtt: Image applied as preview; it stays until replaced or cleared
I (19071) ControlMqtt: Image applied as preview; it stays until replaced or cleared
E (38491) jpeg_to_image: Failed to allocate memory for JPEG output buffer
E (38501) ControlMqtt: JPEG decode failed: ESP_ERR_NO_MEM
E (52901) LcdDisplay: CreateCoverBackground: PNG decode failed
E (58991) LcdDisplay: CreateCoverBackground: OOM (460800 bytes)
E (58991) LcdDisplay: SetBackgroundImage: failed to prepare wallpaper
```

### 9.3 结论

1. **任务栈修复有效**：不再出现 `Failed to create image download task`；
   640×480 PNG 预览、320×240 PNG 预览、160×120 / 240×180 JPEG 预览均能真正应用。
2. **大图 JPEG 仍受 PSRAM 连续内存限制**：240×180 JPEG（输出约 86 KB）可通过，
   320×240 JPEG（输出约 153 KB）开始失败；640×480 JPEG（约 614 KB）失败。
3. **壁纸路径固定需要 460,800 字节连续缓冲**（480×480 RGB565），当前 4B 无法分配；
   即使源图只有 240×180，也会在 cover 转换阶段 OOM。PNG 壁纸还会额外经过
   ARGB8888 解码，640×480 时解码阶段即失败。
4. 404 与无效主机错误码正确；`clear:true` 始终可用。

### 9.4 后续修复方向

- 在 `LcdDisplay` 初始化阶段（PSRAM 尚未碎片化时）预分配一块壁纸缓冲，并提供非 owning
  的 `LvglImage` 包装，避免每次 `CreateCoverBackground` 再申请 460 KB。
- 或改用 LVGL 的 cover 缩放/裁剪直接渲染原图，去掉中间 RGB565 缓冲。
- 大图 JPEG 可考虑按屏幕尺寸降采样解码，或把 JPEG 输出缓冲改为可复用的 PSRAM 池。

### 9.5 480×480 专项修复结果（最终）

为满足“最大支持 480×480”的目标，在 4B 板级配置中调整 PSRAM 布局：

```json
"CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y",
"CONFIG_SPIRAM_RODATA=n"
```

保留代码从 PSRAM 取指，只把只读数据留在 flash。这样 PSRAM 空闲从约 265 KB 提升到约 1.08 MB，最大连续块约 917 KB，足以容纳 480×480 RGB565（460,800 字节）及壁纸 cover 缓冲。

| 用例 | 结果 | 说明 |
|---|---|---|
| 480×480 JPEG preview | **PASS** | `preview_active=true`，串口 `Image applied as preview` |
| 480×480 JPEG background | **PASS** | `wallpaper_active=true`，串口 `Image applied as wallpaper` |
| 480×480 PNG preview | **PASS** | `preview_active=true` |
| 480×480 PNG background | **FAIL** | `CreateCoverBackground: PNG decode failed`；PNG 需先解为 480×480 ARGB8888（约 921 KB），仍超出 LVGL 解码可用内存 |
| 640×480 JPEG preview | **PASS** | 上一轮同配置已验证 |
| `clear:true` | **PASS** | 预览与壁纸均清除 |
| `listen start/stop` | **PASS** | 调整后音频会话仍正常，无 watchdog |

结论：

- **480×480 JPEG 的预览和壁纸均已可用**，满足主要图片下发场景。
- 480×480 PNG 预览可用；**PNG 壁纸仍受 LVGL PNG 解码内存限制**（ARGB 缓冲约 921 KB），需要单独优化 PNG 解码路径或增大 LVGL 解码池。
- 本次只对 4B 关闭 `CONFIG_SPIRAM_RODATA`；`CONFIG_SPIRAM_FETCH_INSTRUCTIONS` 仍为 `y`，尽量保留取指性能。
- 3.49 V2 等其他板型不受该 4B 配置影响。

---

## 10. 其他功能回归检查（2026-11-06 13:29–13:32）

针对 4B 的最终配置：

```json
"CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y",
"CONFIG_SPIRAM_RODATA=n"
```

以及图片任务 PSRAM 栈、notify 20 s 超时，逐项回归如下。

### 10.1 启动与稳定性

| 项 | 结果 |
|---|---|
| 冷启动/复位启动 | 正常，约 8 s 到 idle/控制通道在线 |
| 固件版本 | `2.5.2.1`，`hello` 正常 |
| PSRAM 空闲 | 约 1.08 MB（调整前约 265 KB） |
| PSRAM 最大连续块 | 约 917 KB（调整前约 106 KB） |
| 内部 SRAM 空闲 | 约 17–20 KB，最低约 3 KB |
| watchdog/panic/assert | 本次回归日志未见 |
| WiFi/控制通道 | 正常连接、重连；`mqtt_connected=true` |
| RTC | 启动校时正常 |

### 10.2 已回归功能

| 功能 | 结果 |
|---|---|
| `query_status` 全字段 | PASS |
| 音量设置/恢复、150→100 钳位 | PASS |
| 背光设置/恢复 | PASS |
| 明暗主题切换 | PASS |
| 状态栏文字/颜色 | PASS（视觉未拍照） |
| 唤醒词开关 | PASS（状态位正确） |
| IMU 开关 | PASS（状态位正确） |
| 省电模式（Idle 下 on/off） | PASS |
| `listen start` → connecting → listening | PASS |
| `listen stop` | 协议回执 PASS；主云可能继续 TTS 并回到 speaking/listening，非本次改动引入 |
| `system reboot` → `hello` | PASS |
| 480×480 JPEG 预览 | PASS |
| 480×480 JPEG 壁纸（先清预览） | PASS |
| 480×480 PNG 预览 | PASS |
| 480×480 PNG 壁纸 | FAIL（既有 LVGL PNG 解码内存限制） |
| 480×480 预览 + 壁纸同时存在 | FAIL（两份 460 KB 缓冲 + JPEG 解码缓冲超过 1.08 MB，属容量边界） |
| `notify` 控制语义/取消 | 未变化；外部 CDN 拉流仍失败，20 s 超时已生效 |

### 10.3 未覆盖/需人工确认

- **实际唤醒词命中**：本次只确认 `wake_word_running=true`、AFE/MultiNet 初始化正常，未对麦克风说“你好，邵怡”。
- **TTS/喇叭实际出声**：`listen` 能进入会话并产生 `speaking`/`interaction`，但没有可用的本地音频源确认扬声器输出；外部 `notify` 仍因 CDN 拉流失败。
- **屏幕视觉**：主题、状态栏、图片标志位正常，但未拍照/截图确认实际像素效果；PNG 壁纸失败为串口 `CreateCoverBackground: PNG decode failed`。
- **物理交互**：BOOT 单击/长按/双击、屏幕双击/三击、IMU 晃动、RTC 显示需人工验证。
- **OTA/资产升级**：本次未执行升级流程。
- **性能基准**：`CONFIG_SPIRAM_RODATA=n` 后只读数据从 flash 读取，启动时间与功能未见异常，但未做 CPU/帧率/音频延迟基准对比。

### 10.4 结论

- 本次改动未发现对控制通道、显示、音频会话、WiFi、RTC、重启等功能的**功能性破坏**。
- 唯一明确的新容量边界是：480×480 预览与壁纸**同时存在**时内存不足；单独使用预览或壁纸均可。
- `notify` 的 20 s 超时只影响播报失败场景的等待时间，不影响其他控制指令。
- `CONFIG_SPIRAM_RODATA=n` 是 4B 专属权衡：用少量 flash 读取性能换取约 0.8 MB PSRAM，保证 480×480 JPEG 图片缓冲；3.49 V2 不受影响。

### 10.5 壁纸与公司 logo 层级修复

现象：下发 480×480 壁纸后，之前的公司 logo 仍然显示，并且盖在壁纸上层。

原因：logo 是 `container_` 的子对象，而壁纸是 `container_` 自身的背景样式；LVGL 子对象天然绘制在父对象背景之上。`ShowCompanyLogo()` 之前只负责“显示 logo”，没有考虑远程壁纸是否激活。

修复（[`custom_lcd_display.cc`](../main/boards/waveshare/esp32-s3-touch-lcd-4b/custom_lcd_display.cc)）：

- 新增 `std::atomic<bool> wallpaper_active_`。
- 重写 `SetBackgroundImage()`：先置位，再调用基类；成功后隐藏 `logo_image_`。
- 重写 `ClearBackgroundImage()`：清壁纸后复位并调用 `ShowCompanyLogo()`。
- `ShowCompanyLogo()` 开头检查 `wallpaper_active_`，壁纸激活时直接隐藏 logo（同时覆盖 `SetTheme()`、`SetEmotion()` 的重复调用）。

串口验证：

```
CustomLcdDisplay: Wallpaper active: company logo hidden
CustomLcdDisplay: Wallpaper cleared: company logo restored
```

复测：480×480 JPEG 壁纸下发成功；壁纸激活时切换 light/dark 主题，logo 保持隐藏；`clear:true` 后 logo 恢复。当前设备已保留壁纸显示，未下发 clear。

### 10.6 小智表情位置调整

需求：表情默认在屏幕正中，正好盖住居中的公司 logo；希望上移或下移半个“中心到边缘”的距离，即停在屏幕上方或下方 1/4 处。

实现（[`custom_lcd_display.cc`](../main/boards/waveshare/esp32-s3-touch-lcd-4b/custom_lcd_display.cc)）：

```cpp
const int kEmojiVerticalOffset = -(LV_VER_RES / 4);  // 上移 1/4 屏高
lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, kEmojiVerticalOffset);
```

- 4B 屏高 480，偏移 `-120 px`，表情中心从 y=240 移到 y=120（上方 1/4）。
- 如要改到下方 1/4，把偏移改为 `+(LV_VER_RES / 4)` 即可。
- 只调整 `emoji_box_`，不影响 logo、壁纸、预览图、状态栏和手势事件。
- 壁纸激活时 logo 仍隐藏；表情上移后不再压住居中的 logo。

构建：`xiaozhi.bin=0x2db160`，已烧录。

---

## 11. 语音交互时屏幕偶发抖动：日志分析（2026-11-06 14:01–14:10）

### 11.1 现场日志

语音唤醒并保持 listening 一段时间后，串口出现连续 `task_wdt`：

```text
E (349088) task_wdt: Task watchdog got triggered.
E (349088) task_wdt:  - IDLE0 (CPU 0)
E (349088) task_wdt: CPU 0: audio_input
E (349088) task_wdt: CPU 1: IDLE1
Backtrace:
  dios_ssp_aec_res_get_residual_echo
  dios_ssp_aec_res_process
  dios_ssp_aec_process_api
  afe_feed
  AfeAudioEngine::Feed()
  AudioService::AudioInputTask()

E (369088) task_wdt: CPU 0: esp_timer   (LVGL tick)
E (379088) task_wdt: CPU 0: wifi
```

`addr2line` 解码确认：热点在 AEC 的 `dios_ssp_aec_res_process`，即残留回声抑制 DSP。`IDLE0` 被 `audio_input` 长时间占满，`esp_timer`（LVGL tick）和 WiFi 任务也被拖延，与屏幕内容抖动的时间吻合。

### 11.2 根因

`Application` 已经把 `aec_required` 限制为状态相关，但 `AfeAudioEngine::ApplyAfeControls()` 仍用：

```cpp
enable_aec = device_aec_enabled && (aec_required || kVoiceProcessingEnabled)
```

Listening 下 `kVoiceProcessingEnabled` 为真，所以即使 `aec_required=0`，AEC 仍然开着：

```text
AfeAudioEngine: AEC on (wake=0 voice=1 required=0)
```

Listening 阶段本地没有 TTS/播报播放，AEC 没有实际回声可消，却持续占用 core 0，导致：
- `IDLE0` 饿死，task_wdt；
- `esp_timer`/LVGL tick 延迟；
- RGB 刷新/动画抖动。

### 11.3 修复

1. `main/application.cc`：AEC 状态门控只保留 `Speaking / Notifying`。
2. `main/audio/engines/afe_audio_engine.cc`：

```cpp
const bool enable_aec = device_aec_enabled_.load() && aec_required_.load();
```

不再把 `kVoiceProcessingEnabled`（Listening 也为真）当作 AEC 开启条件。

### 11.4 验证

- 连续 listening 60 s：**0 次 `task_wdt`**，内部 SRAM 稳定在约 16–20 KB。
- Listening 下不再出现 `AEC on`；进入 Speaking/Notifying 时仍由 `aec_required` 打开 AEC，回声消除路径保留。
- 构建：`xiaozhi.bin=0x2db160`，已烧录。
- 语音唤醒/聆听状态机、控制通道、屏幕显示未见异常。

### 11.5 仍需确认

本次复现的是 **Listening 阶段**的 AEC 过载。Speaking/Notifying 阶段 AEC 仍会开启（这是必要的），如果实际 TTS 播放时仍出现抖动，需要继续观察该阶段的 `task_wdt`/RGB DMA 日志。

---

## 12. 播放音乐时双击屏幕进入聆听（2026-11-06）

### 12.1 问题

播放音乐/TTS 时设备处于 `Speaking`，状态栏显示“说话中…”。此时：
- 三击切主题正常；
- 双击没有响应，只能按 BOOT 或说唤醒词进入聆听。

### 12.2 原因

`CustomLcdDisplay::ResolveDoubleClickAction()` 原来只给 `Idle` 返回 `kStartListening`；`Speaking` 直接返回 `kNone`，所以双击被吞掉。`RunDoubleClickAction()` 里还有一层 `state == Idle` 检查。

### 12.3 修改

- `ResolveDoubleClickAction()`：`Idle` 或 `Speaking` 都返回 `kStartListening`。
- `RunDoubleClickAction()`：允许 `Idle` 或 `Speaking`；`Speaking` 下调用
  `StartListening(GetDefaultListeningMode())`。
- `Application::HandleStartListeningEvent()` 已有 `Speaking` 分支：
  `AbortSpeaking()` → `SetListeningMode()`，等价于语音打断。

因此播放中双击会：
1. 打断当前音乐/TTS；
2. 进入 `Listening`，状态栏显示“聆听中…”；
3. 按当前 AEC 模式决定 `AutoStop` / `Realtime`。

### 12.4 说明

- 如果双击时屏幕上有常驻预览图，仍优先执行“清除预览图”，这是原有行为。
- `Notifying`（远程播报）双击仍优先“停止播报”，不会直接进入聆听；再双击一次可进入。
- 构建：`xiaozhi.bin=0x2db170`，已烧录。
- 需要真机触摸确认；本环境无法模拟物理双击。

### 12.5 与音乐闪烁的关系

BOOT 双击关闭 AEC 后音乐不再闪烁，确认闪烁与 AEC DSP 负载相关。播放中双击进入 `Listening` 后，当前 AEC 门控会在 Listening 关闭 AEC，因此进入聆听后不会再因 AEC 抖动；但**音乐刚开始播放、尚处于 Speaking 的 3–4 秒**仍可能闪。要彻底消除这段，需要单独处理 Speaking 阶段的 AEC（服务端 AEC、音乐模式关闭 AEC，或降低 AEC 负载）。

### 10.7 唤醒词阈值调整为 20%

- 4B `config.json`：`CONFIG_CUSTOM_WAKE_WORD_THRESHOLD` 15 → **20**。
- 重新构建后 `assets/index.json` 生成 `"threshold": 0.2`，并同时烧录 app 与 `generated_assets.bin`。
- 启动日志确认实际生效值：

```text
CustomWakeWord: Command: ni hao shao yi, Text: 你好，邵怡, Action: wake
set det threshold to 0.200000
```

- 构建：`xiaozhi.bin=0x2db210`，已烧录；设备重启后 `hello` 正常、`wake_word_running=true`。
- 注意：只烧 app 不会改 assets 内的阈值；本次已重新生成并烧录 assets。
