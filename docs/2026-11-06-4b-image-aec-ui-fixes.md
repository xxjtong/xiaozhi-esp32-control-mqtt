# 2026-11-06：4B 图片容量、AEC 抖动与显示交互修复

> 目标板型：Waveshare ESP32-S3-Touch-LCD-4B
> 固件：`PROJECT_VER = 2.5.2.1`，ESP-IDF v6.1
> 最终构建：`xiaozhi.bin = 0x2db210`（分区余量 27%）
>
> 接续 [2026-11-06-4b-audio-flicker-lock-fix.md](2026-11-06-4b-audio-flicker-lock-fix.md)
> 与 [control-mqtt-test-2026-11-06.md](control-mqtt-test-2026-11-06.md)。

## 1. 变更总览

| 问题 | 处理 | 主要文件 | 验证 |
|---|---|---|---|
| 4B `set_image` 创建任务失败 | 图片任务栈放 PSRAM，保留内部 RAM 回退；失败补 `extra.error` | `main/control_mqtt.cc` | 480×480 JPEG 预览/壁纸通过 |
| PSRAM 只有约 265 KB，480×480 图片 OOM | 4B 关闭 `CONFIG_SPIRAM_RODATA`，保留代码取指在 PSRAM；空闲提升到约 1.08 MB | `main/boards/waveshare/esp32-s3-touch-lcd-4b/config.json` | 480×480 JPEG 预览/壁纸通过 |
| `notify` 5 s 头超时必然失败 | HTTP 超时 5 s → 20 s | `main/notify/notify_player.cc` | 超时生效；外网 CDN 仍受链路限制 |
| 播放音乐约 9–10 s 后屏幕闪烁 3–4 s | Listening 阶段 AEC 无条件开启，AEC DSP 饿死 IDLE0/esp_timer | `main/application.cc`、`main/audio/engines/afe_audio_engine.cc` | 连续 Listening 60 s 无 `task_wdt` |
| 壁纸被公司 logo 盖住 | 壁纸激活时隐藏 logo，清壁纸后恢复 | `main/boards/waveshare/esp32-s3-touch-lcd-4b/custom_lcd_display.*` | 壁纸/清壁纸串口日志通过 |
| 小智表情居中压住 logo | 表情上移到屏幕上方 1/4（480 屏为 y=120） | `custom_lcd_display.cc` | 编译烧录；待真机视觉确认 |
| 播放中双击无响应 | Speaking 双击进入 Listening；Listening 双击回到 Idle | `custom_lcd_display.cc` | 播放中双击进入聆听已由用户确认；Listening→Idle 待触摸确认 |
| 唤醒阈值偏严 | 15% → 20% | `config.json` + assets `index.json` | 启动日志 `set det threshold to 0.200000` |

## 2. 图片与 PSRAM

### 2.1 图片任务栈

`HandleSetImage()` 原来使用 12 KB 内部 RAM 栈；4B 在 AFE 初始化后没有足够连续内部 RAM：

```text
E ControlMqtt: Failed to create image download task
```

现改为：

- 优先 `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`；
- PSRAM 不可用时回退 `xTaskCreate`；
- 任务删除按栈来源选择 `vTaskDeleteWithCaps` / `vTaskDelete`；
- 创建失败回 `extra.error="failed to create image task"`。

### 2.2 4B PSRAM 配置

4B 只关闭只读数据的 PSRAM 复制：

```json
"CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y",
"CONFIG_SPIRAM_RODATA=n"
```

| 指标 | 调整前 | 调整后 |
|---|---|---|
| PSRAM 空闲 | 约 265 KB | 约 1.08 MB |
| PSRAM 最大连续块 | 约 106 KB | 约 917 KB |
| 启动时间 | 约 8 s | 约 8 s |

### 2.3 能力边界

- 480×480 JPEG 预览：通过；
- 480×480 JPEG 壁纸：通过（先清旧壁纸）；
- 480×480 PNG 预览：通过；
- 480×480 PNG 壁纸：仍失败，PNG 需 ARGB8888 约 921 KB；
- 预览 + 壁纸同时存在：可能 OOM，建议先 `clear`；
- 640×480 JPEG 预览：通过；更大尺寸不保证；
- 壁纸常驻占用 480×480 RGB565 = 460,800 字节 PSRAM，重启不保留。

## 3. AEC 与屏幕抖动

### 3.1 日志

语音唤醒并保持 Listening 后出现：

```text
task_wdt: - IDLE0 (CPU 0)
task_wdt: CPU 0: audio_input
Backtrace:
  dios_ssp_aec_res_process
  afe_feed
  AfeAudioEngine::Feed()
  AudioService::AudioInputTask()
```

`addr2line` 确认热点为 AEC 残留回声抑制 DSP。Listening 阶段 `kVoiceProcessingEnabled=1` 导致 AEC 仍被打开：

```text
AEC on (wake=0 voice=1 required=0)
```

### 3.2 修复

- `Application` 只在 `Speaking / Notifying` 设置 `aec_required`；
- `AfeAudioEngine::ApplyAfeControls()` 改为：
  ```cpp
  const bool enable_aec = device_aec_enabled_.load() && aec_required_.load();
  ```
- Listening/Idle 不再运行无意义的 AEC，Speaking/Notifying 仍保留回声消除。

### 3.3 验证

- 连续 Listening 60 s：0 次 `task_wdt`；
- 播放音乐时 BOOT 双击关闭 AEC 可消除闪烁，确认与 AEC 负载相关；
- 播放中双击进入 Listening 后 AEC 关闭，不再持续抖动；
- Speaking 刚开始的 3–4 s 仍可能受 AEC 影响，彻底消除需服务端 AEC 或音乐模式策略。

## 4. 显示与手势

### 4.1 壁纸与 logo 层级

logo 是 `container_` 的子对象，壁纸是父对象背景，LVGL 子对象天然盖在背景上。新增：

- `std::atomic<bool> wallpaper_active_`；
- `SetBackgroundImage()` 成功后隐藏 logo；
- `ClearBackgroundImage()` 后恢复 logo；
- `ShowCompanyLogo()` 在壁纸激活时直接隐藏，覆盖主题/表情重复调用。

### 4.2 表情位置

```cpp
const int kEmojiVerticalOffset = -(LV_VER_RES / 4);
lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, kEmojiVerticalOffset);
```

480 屏上表情中心从 y=240 移到 y=120（上方 1/4），不再压住居中 logo。改到下方 1/4 只需把符号改为 `+`。

### 4.3 双击状态切换

| 当前状态 | 双击结果 |
|---|---|
| `Idle` | → `Listening` |
| `Listening` | → `Idle` |
| `Speaking` | → `Listening`（先打断播放） |
| `Notifying` | 停止播报 → `Idle` |

三击仍切换 light/dark 主题；有常驻预览图时双击优先清图。

## 5. 唤醒阈值

- 4B：`CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20`；
- `assets/index.json`：`"threshold": 0.2`；
- 启动日志：`set det threshold to 0.200000`；
- 最近一次实测唤醒 `prob=0.221163`，20% 阈值余量约 2 个百分点；如漏唤醒建议回 18%，如误唤醒再升 22–25%。

## 6. 验证与待确认

已完成：

- 4B 构建、`idf.py flash`（app + assets）；
- 480×480 JPEG 预览/壁纸；
- 壁纸/清壁纸、logo 隐藏/恢复；
- 连续 Listening 60 s 无 `task_wdt`；
- 唤醒阈值 20% 启动日志；
- 控制通道、音量/亮度/主题/IMU/重启回归；
- Host 侧 `python3 -m unittest discover -s scripts/tests -v`。

待真机确认：

- 表情上移 1/4 的视觉效果；
- Listening 双击回到 Idle（最新固件尚未物理双击验证）；
- Speaking 阶段 3–4 s 闪烁是否仍存在（AEC 仍开启）；
- PNG 480×480 壁纸。

## 7. 相关文档

- 控制通道报文与测试：[control-mqtt-api.md](control-mqtt-api.md)、[control-mqtt-test-2026-11-06.md](control-mqtt-test-2026-11-06.md)
- 固件定制 API：[firmware-custom-api.md](firmware-custom-api.md)
- 先前音频/锁修复：[2026-11-06-4b-audio-flicker-lock-fix.md](2026-11-06-4b-audio-flicker-lock-fix.md)
