# 威思客定制版 2.4.2.1：需求与真机测试记录

> 日期：2026-10-26  
> 固件版本：`2.4.2.1`  
> 测试设备：Waveshare ESP32-S3-Touch-LCD-3.49 V2  
> 设备 UUID：`0d0cef91-ec46-41c0-9745-a6ee1e2d364c`  
> MAC：`80:45:6b:35:c8:9c`

---

## 1. 版本号策略

`PROJECT_VER` 从 `2.4.2` 调整为：

```text
2.4.2.1
```

含义：

- `2.4.2`：当前 fork 的上游基础版本；
- `.1`：威思客 fork 修订号，后续每次发布递增 `.2`、`.3`。

选择数字第四段而不是 `2.4.2-virsical.1`，是因为现有 OTA 版本解析只接受数字段；带字母的后缀会解析失败，影响版本比较和后续 OTA 恢复。版本号可追溯到上游基础版本，同时与原始项目仓库版本区分。

> 当前代码包含部分上游 v2.5.2 修复，但不是完整 v2.5.2 升级；版本号仍以 `2.4.2` 为上游基线。

---

## 2. 今日需求（2026-10-26）

### 2.1 产品与客户材料

- 目标硬件确定为 **ESP32-S3-Touch-LCD-4B**：4 英寸方形屏、480×480、86.5×86.5×14 mm、5V/1A Type-C 供电、Wi-Fi、双麦克风、双喇叭、触摸屏与侧边按键；当前继续用 3.49 V2 做固件与真机测试。
- 唤醒词统一为“**你好，小威**”，设备 UI 的“轻触说话”改为“你好，小威 / 唤醒并开始对话”。
- 通过威思客服务平台接入第三方智能体：
  - WorkBuddy 企业版
  - 豆包办公
  - 千问 Go
  - 语音发起任务、结果推送到桌面 AI 助理并报备。
- 对接 FM 设施系统：语音报修、自动生成工单、进度查询/完成通知。
- 扩展威思客空间能力：预约会议室/会议、临时会议、访客到访通知、查询温度/空气质量/占用、语音开灯/调空调/场景切换。
- 客户版定位为“所有工位场景”：固定工位、共享工位、混合办公、多园区组织。
- 可扩展功能：工作时长分析、久坐提醒、喝水提醒。
- 删除客户版“建议试点”内容；客户版保持 3 页 A4 浅色风格。
- 产品效果图：增加设备在办公桌面上摆放的场景示意；报修示例改为“3 楼走廊的灯坏了，帮我报修”。
- 架构边界：威思客提供身份/组织/日程；企业服务端只连接威思客与 Agent 平台；小智云支持私有化，未来可与企业服务端合并部署（合并保持语音平面/业务平面逻辑隔离）。

### 2.2 固件与控制通道

- 版本号增加 fork 修订号。
- 3.49 与 4B 的 `config.json` 均补齐“你好，小威”自定义唤醒词配置。
- 测试临时预览图、背景壁纸、清除图片、IMU 开关、主题切换、状态提示。
- 控制背景图的分辨率和文件大小，确认 PNG/JPEG 的可用边界。

### 2.3 下午追加：远程播报闭环与本地停止手势

面向“服务平台下发语音播报”场景（对接 FM 工单、会议提醒、访客通知等）：

- `notify` 补齐送达确认：新增上行 `notify_result`（`outcome` + `duration_ms`）与可选 `id` 对账；
  修复原先无条件回 `success=true` 的回执语义。
- 新增下行 `abort_notify`：远程取消正在播放的播报。
- 本地可停止播报：播报中**屏幕双击**停止（不清预览图、不连带切主题）；BOOT 键与 IMU 姿态变化同样可停止。
- IMU 改为触发**聆听模式**，并抑制屏幕点击/按键造成的机身抖动误触发。
- 详见 [control-mqtt-notify-feedback.md](control-mqtt-notify-feedback.md)。

---

## 3. 测试环境

| 项 | 值 |
|---|---|
| 开发板 | Waveshare ESP32-S3-Touch-LCD-3.49 V2 |
| 芯片 | ESP32-S3，8MB PSRAM |
| 固件 | `2.4.2.1`，首版构建 2026-10-26 12:01；下午追加播报闭环后重新构建并刷入（15:00 左右） |
| 应用大小 | 首版 `0x2eef20`（3,075,872 B）；播报闭环版 `0x2f0040`，分区余 25% |
| Flash 方式 | 写入 OTA_0 + OTA_1，保留 NVS/WiFi 配置 |
| 网络 | Wi-Fi `Temi`，IP `192.168.6.76` |
| 主协议 | 已连接小智云 MQTT |
| 第二路控制 | 已连接 `mqtt.virsical.cn:21883`，hello 已发送 |

---

## 4. 真机测试结果

### 4.1 启动与基础外设

| 项目 | 结果 |
|---|---|
| 3.49 V2 屏幕 / LVGL | ✅ |
| 触摸 | ✅ |
| QMI8658 IMU | ✅ |
| PCF85063 RTC | ✅ |
| ES8311 / ES7210 音频 | ✅ |
| Wi-Fi / IP | ✅ |
| 主云 MQTT | ✅ |
| 第二路控制 MQTT | ✅ |
| 启动版本日志 | `App version: 2.4.2.1` |

### 4.2 控制命令

| 命令 | 结果 |
|---|---|
| `query_status` | ✅ 返回 idle、电量、网络、主题、音量、亮度、IMU、唤醒词、MQTT 状态 |
| `set_status` | ✅ 顶部状态栏成功显示 |
| `set_imu false/true` | ✅ `imu_enabled` 相应变化 |
| `set_theme dark/light` | ✅ `theme` 相应变化 |
| `set_image` 临时预览 JPEG | ✅ `preview_active=true` |
| `set_image` 临时预览 PNG | ✅ `preview_active=true` |
| `set_image` 背景 JPEG | ✅ |
| `set_image` 背景 PNG | ✅（受尺寸限制） |
| `set_image clear` | ✅ `preview_active=false`、`wallpaper_active=false` |
| 双击清图 | ✅ 真机通过 |
| 三击切主题 | ✅ 真机通过 |

### 4.3 远程播报闭环（下午追加）

| # | 用例 | 结果 |
|---|---|---|
| 1 | `query_status` 新字段 | ✅ `notify_active`、`notify_id` 返回正确 |
| 2 | 正常播报 | ✅ `outcome=finished duration=5416ms`（4.9s 音频） |
| 3 | 播报中 `abort_notify` | ✅ `outcome=cancelled duration=2161ms` |
| 4 | 空闲时 `abort_notify` | ✅ `success=false`，正确拒绝 |
| 5 | 唤醒打断 | ✅ 说「你好，小威」→ `outcome=aborted duration=15424ms` → connecting → listening → speaking |
| 6 | 播报中屏幕双击 | ✅ `event notify_stopped=double_click` + `outcome=user_stopped duration=2313ms`（37s 音频 2.3s 掐断）；无 `theme_changed` |
| 7 | IMU 姿态触发聆听 | ✅ 空闲晃动进聆听；播报中晃动 → `motion_detected` → `outcome=user_stopped` → listening |
| 8 | 喇叭振动是否误触发 IMU | ✅ 全程不触碰设备，`motion_detected` 0 次，完整播完 43692ms |
| 9 | 点击抖动抑制 | ✅ 双击屏幕时机身抖动未产生 `motion_detected` |
| 10 | 播放失败场景 | ✅ 同样上报 `notify_result`（`duration=5080 / 283ms`） |

**唤醒词是「你好，小威」**（`CONFIG_CUSTOM_WAKE_WORD_DISPLAY`），不是「你好小智」——首次测用例 5
失败就是因为说错了唤醒词。播报期间 `wake_word_running` 实测为 `true`。

**排障要点**：`preview_active`（= `HasPreviewImage()`）**不等于“图片可见”**；排查点击类手势直接看串口
`CustomLcdDisplay` 日志最快，MQTT 事件只在成功时才发，缺席无法区分“没触发”和“没操作”。

### 4.4 背景图分辨率/大小矩阵

测试图片来自公网占位图服务，文件本身很小，失败与文件体积无关：

| 格式 | 分辨率 | 文件大小 | 结果 |
|---|---|---:|---|
| JPEG | 640×172 | 7.3KB | ✅ `wallpaper_active=true` |
| PNG | 800×450 | 3.6KB | ✅ `wallpaper_active=true` |
| PNG | 1024×576 | 4.7KB | ❌ `failed to apply background image` |
| JPEG | 1280×720 | 29KB | ✅ `wallpaper_active=true` |
| PNG | 640×172 | 2.5KB | ✅ `wallpaper_active=true`（当前保留） |

**结论：**

- 临时预览图：JPEG/PNG 800×450 都正常；
- PNG 背景在 3.49 V2 上的实测上限约为 **800×450**；1024×576、1200×800 均失败；
- JPEG 背景可支持到 1280×720；
- 失败原因是 PNG 解码后的像素内存，不是文件大小；
- 大图背景优先使用 JPEG，或把 PNG 控制在 800×450 以内。

### 4.5 并发保护

连续快速下发多条 `set_image` 时，设备返回：

```json
{"success":false,"extra":{"error":"another image download is in progress"}}
```

这是单飞保护，属于预期行为；等待上一条回执后再下发即可。

---

## 5. 已知问题与限制

1. **大尺寸 PNG 背景失败**：1024×576/1200×800 失败，同尺寸 JPEG 正常；建议限制 PNG 背景 ≤800×450，或后续优化 PNG 解码内存。
2. **4B 尚未完成功能对齐**：4B 板级目录已存在，但 480×480 工牌 UI、手势、IMU/RTC、电量展示仍需在 4B 上验证；4B 尚未全量构建/真机测试。
3. **唤醒词文案**：实际命令为 `ni hao xiao wei`，运行日志中的模型文案仍显示“你好小威”（无逗号），不影响唤醒；如要求屏幕/日志严格带逗号，需要调整唤醒词模型 JSON。
4. **OTA**：当前 fork 仍禁用实际升级执行，版本号仅用于标识；未做 OTA 升级链路验证。
5. **手势**：双击/三击已真机验证。**播报中双击改为停止播报**（不清预览图，也不连带触发三击切主题），空闲时双击仍是清预览图、三击切主题。
6. **IMU**：0.1g 阈值偏灵敏，已加入点击抑制（屏幕点击/按键后 700ms 忽略运动），但拿放桌面等场景仍可能误触发；可用 `set_imu false` 关闭。
7. **唤醒词**：实测命令为 `ni hao xiao wei`，屏幕/日志文案为“你好，小威”（见第 4.3 节说明）。

---

## 6. 后续待办

- [ ] 4B 板级 bring-up：显示/触摸/音频/PMU/IMU/RTC。
- [ ] 4B 480×480 工牌 UI 与双击/三击手势对齐。
- [ ] PNG 背景上限收紧到 800×450，或优化解码内存并返回明确错误。
- [ ] 4B 全量构建 + 真机测试 + 背景图矩阵复测。
- [ ] 私有化小智云 PoC 与企业服务端合并边界验证。
- [ ] 客户版材料随 4B 实际外观/接口再校对。

---

## 7. 关联材料

- 项目总览：[PROJECT_OVERVIEW.md](../PROJECT_OVERVIEW.md)
- 控制通道协议：[control-mqtt.md](control-mqtt.md)
- 播报闭环实施与测试：[control-mqtt-notify-feedback.md](control-mqtt-notify-feedback.md)
- 远程播报服务设计评估：[tts-broadcast-design.md](tts-broadcast-design.md)
- 烧录实操：[flashing.md](flashing.md)
- 方案评估与架构：[virsical-desktop-ai-badge-architecture.md](virsical-desktop-ai-badge-architecture.md)
- 客户版 PDF：[virsical-desktop-ai-badge-customer-brochure.pdf](virsical-desktop-ai-badge-customer-brochure.pdf)
