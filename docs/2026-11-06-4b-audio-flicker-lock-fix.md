# 2026-11-06：4B 音频优先级、LVGL 锁竞争与唤醒/TTS 稳定性修复

> 目标板型：`waveshare/esp32-s3-touch-lcd-4b`
>
> 接续 [2026-11-05 空闲态激活修复](2026-11-05-4b-idle-activation-fix.md)。本文记录次日
> 真机使用中发现的三个问题：唤醒失败率偏高、TTS 播一半断流但字幕继续、以及
> “连接中卡住 + logo 每 6~7 秒闪一次”。

## 1. 结论

| 现象 | 根因 | 修复 |
|---|---|---|
| TTS 播一半没声、字幕继续滚 | `opus_codec` 优先级只有 2 且未绑核，AEC/MultiNet/LVGL 忙时解码被饿死；20 包（约 1.2s）的 decode queue 填满后新音频被静默丢弃 | 解码/播放优先级 5、固定 core 1；解码队列满时打日志 |
| 唤醒失败率偏高 | `audio_afe`（MultiNet）未绑核，可能落到被 AEC 占满的 core 0；唤醒阈值 0.20 偏严；AEC NLP `VERYAGGR` 把 core 0 吃到接近满载 | `audio_afe` 固定 core 1；阈值 0.20→0.15；NLP `VERYAGGR`→`NORMAL` |
| “连接中”卡住 + logo 6~7s 闪一次 | `CustomLcdDisplay::SetEmotion()`/`SetTheme()` 在 main 任务里**没有持 LVGL 锁**就改对象树，和 LVGL 任务并发导致 TLSF 堆损坏/主任务卡在 `lv_tlsf_free` | 两个入口都加 `DisplayLockGuard` |

修复后真机确认：

- logo 闪烁消失
- 空闲唤醒成功（日志 `prob=0.244994`）
- 长 TTS 完整播放，字幕与音频同步
- 交互窗口（09:39~09:42）**0 次 `task_wdt`**、**0 次 `Decode queue is full`**
- 09:44 之后连续空闲曾周期性打印 `task_wdt`（`audio_input`/`audio_afe` 双核吃满）；
  追加 **状态门控 AEC** 后，10 分钟空闲 **0 次 `task_wdt`**（见 §4.5）

---

## 2. 问题一：TTS 播一半断流，字幕继续

### 2.1 代码链路

```text
UDP 音频包 -> audio_decode_queue（20 包 ≈ 1.2s）
           -> opus_codec 任务（原优先级 2，未绑核）
           -> audio_playback_queue（2 帧）
           -> audio_output 任务（原优先级 4）
           -> ES8311/I2S
```

字幕走另一条链路：

```text
MQTT tts/sentence_start -> Application -> display->SetChatMessage()
```

两条链路完全独立。音频包被丢弃时，字幕仍会继续显示，所以用户看到
“声音停了但字幕还在滚”。

### 2.2 根因

`application.cc` 的 `OnIncomingAudio` 只做一次入队：

```cpp
audio_service_.PushPacketToDecodeQueue(std::move(packet));
```

`PushPacketToDecodeQueue(packet, wait=false)` 在 decode queue 满时直接返回
`false`，**没有日志**。而 `opus_codec` 的任务优先级只有 2：

| 任务 | 原优先级 | 原绑核 |
|---|---|---|
| `audio_input`（AEC feed） | 8 | core 0 |
| `audio_output` | 4 | 不绑 |
| LVGL | 4（本次之前已提到 4） | core 1 |
| `audio_afe`（MultiNet） | 3 | 不绑 |
| `opus_codec`（解码） | **2** | 不绑 |

AEC + MultiNet 接近满载时，解码任务排在所有音频/UI 任务之后；一旦 decode
queue 的 1.2s 缓冲被填满，后续 UDP 包就被静默丢弃。字幕不受影响，继续滚动。

### 2.3 修复

`main/audio/audio_service.cc`：

- `audio_output`：优先级 4→5，`xTaskCreatePinnedToCore(..., 1)`。
- `opus_codec`：优先级 2→5，`xTaskCreateStaticPinnedToCore(..., 1)`；PSRAM 栈
  分配失败时回退到同样优先级的内部栈。
- 两者固定 core 1，与 core 0 的 AEC 分开。

`PushPacketToDecodeQueue` 增加丢包日志（每秒最多一条）：

```text
AudioService: Decode queue is full, dropping audio packet (dropped N so far)
```

这样以后再出现断流，可以直接区分“丢包”与“状态切换/解码失败”。

> 解码队列容量仍保持 20 包。优先保证内部 SRAM 给 AFE；优先级 5 正常时不需要
> 更大缓冲。若未来网络抖动导致丢包，可再把
> `MAX_DECODE_PACKETS_IN_QUEUE` 从 20 提到 30~40。

---

## 3. 问题二：唤醒失败率偏高

### 3.1 现场日志

成功唤醒时概率大多在 0.23~0.27：

```text
Custom wake word detected: command_id=1, string= ni hao shao yi, prob=0.244994
Wake word detected: 你好，邵怡 (state: 3)
StateMachine: State: idle -> connecting
```

失败尝试没有日志（MultiNet 只在命中时返回结果），但和以下现象同时出现：

- `task_wdt`：`IDLE0` 被 `audio_input` 饿死，`IDLE1` 偶尔被 `audio_afe` 占用
- 用户观察到的 logo 闪烁
- AEC 高配的 backtrace 热点在 `dios_ssp_aec_res_process`

### 3.2 修复

1. **`audio_afe` 固定 core 1**
   - 文件：`main/audio/engines/afe_audio_engine.cc`
   - `xTaskCreateStatic` → `xTaskCreateStaticPinnedToCore(..., 1)`
   - AEC feed 在 core 0，MultiNet/VAD 在 core 1，避免被调度到已饱和的 core 0。

2. **唤醒阈值 0.20 → 0.15**
   - 文件：`main/boards/waveshare/esp32-s3-touch-lcd-4b/config.json`
   - `CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=15`
   - **注意：阈值在 assets 的 `index.json` 里，必须同时烧 app + `generated_assets.bin`
     （0x800000）**，只烧 app 不会生效。
   - 代价：可能增加误唤醒；若误唤醒明显，回退到 18。

3. **AEC NLP `VERYAGGR` → `NORMAL`**
   - 文件：`main/audio/engines/afe_audio_engine.cc`
   - AEC 主滤波器仍保留，只降低残留回声抑制强度。
   - 目的：降低 core 0 的 AEC DSP 占用，给 AFE feed/MultiNet 留余量。
   - 代价：残留回声抑制变弱；若长 TTS 时出现自打断/回声，回退到 `AGGR` 或
     `VERYAGGR`。

---

## 4. 问题三：连接中卡住 + logo 每 6~7 秒闪一次

### 4.1 看门狗 backtrace

设备卡在“连接中”，每 10s 打印：

```text
task_wdt: - IDLE0 (CPU 0)
task_wdt: Tasks currently running:
task_wdt: CPU 0: main
task_wdt: CPU 1: IDLE1
```

解码 backtrace：

```text
CustomLcdDisplay::SetEmotion(char const*)
  -> lv_obj_remove_flag()
  -> lv_obj_mark_layout_as_dirty()
  -> lv_display_send_event()
  -> lv_event_send()
  -> cleanup_event_list()
  -> lv_free()
  -> lv_tlsf_free()
```

主任务在 `SetEmotion` 里卡死/自旋，不是网络问题。

### 4.2 根因

`CustomLcdDisplay::SetEmotion()` 原来的实现：

```cpp
void CustomLcdDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) {
        if (emoji_box_ != nullptr) lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    ...
    if (emoji_box_ != nullptr) lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    RgbLcdDisplay::SetEmotion(emotion);
}
```

`SetEmotion()` 由 main 任务在 `HandleStateChangedEvent()` 里调用；`lv_obj_add_flag` /
`lv_obj_remove_flag` 直接改 LVGL 对象树，**没有持有 LVGL 锁**。LVGL 任务同时在
重绘/处理事件，两个任务并发改 TLSF 堆，导致：

- `lv_tlsf_free` 卡死/堆损坏
- 主任务在“连接中”状态切换时自旋，状态机停在 connecting
- 屏幕周期性闪烁（LVGL 事件/对象树异常）
- 看门狗每 10s 触发

`SetTheme()` 里的 `ApplyBottomLayer()` 也有同样问题：远程 `set_theme` 从 main
任务进入，直接改 `container_`/`lv_screen_active()` 的样式，没有锁。

### 4.3 修复

`main/boards/waveshare/esp32-s3-touch-lcd-4b/custom_lcd_display.cc`：

```cpp
void CustomLcdDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);   // 新增
    ...
}

void CustomLcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);   // 新增
    RgbLcdDisplay::SetTheme(theme);
    ApplyBottomLayer();
    ShowCompanyLogo();
}
```

`LcdDisplay::SetEmotion` / `SetTheme` 内部也会加锁，`lvgl_port` 的锁是递归互斥量，
因此重复加锁安全。`ShowCompanyLogo()` 原本就有锁，也安全。

### 4.4 验证

- 交互窗口（30s 空闲 + 长对话 + 多次唤醒）：**0 次 `task_wdt`**
- 09:44 之后连续空闲：仍周期性出现 `task_wdt`（`CPU 0: audio_input` /
  `CPU 1: audio_afe`），非 panic，不改状态、不中断音频
- logo 闪烁消失
- “连接中”不再卡死；网络失败时能正常回到 idle/错误提示
- 长 TTS 完整播放，字幕与音频同步
- 空闲唤醒日志：`prob=0.244994`，进入 `idle -> connecting -> listening`

### 4.5 追加：状态门控 AEC（2026-11-06 下午）

锁修复后长时空闲仍有 `task_wdt`（`audio_input`/`audio_afe` 双核吃满）。根因：
AEC 在 Idle 仍随 `kWakeWordEnabled` 开启；空闲没有播放，AEC 没有回声参考可消，
却持续占 core 0 的 DSP。

改动：

- `AudioEngine` 新增 `SetAecRequired(bool)`（Lite 引擎默认 no-op）；
- `AfeAudioEngine::SetAecRequired()` 设置 `aec_required_`，由 `ApplyAfeControls()` 应用：
  ```cpp
  const bool enable_aec = device_aec_enabled_.load() &&
                          (aec_required_.load() || (bits & kVoiceProcessingEnabled));
  ```
  并在状态变化时打印 `AEC on/off (wake/voice/required)`；
- `AudioService::SetAecRequired()` 记住并下发给引擎；
- `Application::HandleStateChangedEvent()` 按状态设置：
  - `Listening / Speaking / Notifying` → `true`
  - `Idle / Connecting / Activating / ...` → `false`
- **MultiNet 不受影响**：Idle 下仍 `EnableWakeWordDetection(true)`，AFE feed/fetch
  持续运行；关的只是 AEC 这一级 DSP。

10 分钟空闲 + 多轮对话验证：

```text
AEC on  (wake=0 voice=1 required=1)   # Listening
AEC off (wake=1 voice=0 required=0)   # Idle，MultiNet 仍在跑
Custom wake word detected ... prob=0.189964
Custom wake word detected ... prob=0.268940
Custom wake word detected ... prob=0.165524
```

- `task_wdt`：**0 次**
- `Decode queue is full`：**0 次**
- 用户确认：watchdog 消失，唤醒/TTS 正常

> 边界：若 `tts stop` 后播放队列还有约 1~2 帧（~120ms）尾音，此时状态已回 Idle、
> AEC 已关，理论上可能让最后一点尾音进入麦克风。当前实测未出现自唤醒；若以后
> 出现，可加 500ms 尾音保持，或改成“最近播放时间戳”门控。

---

## 5. 改动文件

| 文件 | 改动 |
|---|---|
| `main/audio/audio_service.cc` | 解码/播放优先级 5 + 固定 core 1；decode queue 丢包日志；`SetAecRequired` 存储/下发 |
| `main/audio/audio_service.h` | `decode_drop_count`、`last_decode_drop_log_time_`、`aec_required_` |
| `main/audio/audio_engine.h` | 新增 `SetAecRequired(bool)` 虚接口（Lite 默认 no-op） |
| `main/audio/engines/afe_audio_engine.cc` / `.h` | `audio_afe` 固定 core 1；AEC NLP `NORMAL`；`aec_required_` 门控与 `AEC on/off` 日志 |
| `main/application.cc` | `HandleStateChangedEvent` 按 Listening/Speaking/Notifying 设置 AEC 需求 |
| `main/boards/waveshare/esp32-s3-touch-lcd-4b/custom_lcd_display.cc` | `SetEmotion`/`SetTheme` 加 LVGL 锁 |
| `main/boards/waveshare/esp32-s3-touch-lcd-4b/config.json` | 唤醒阈值 20→15 |

> 唤醒阈值在 assets 分区：烧录时必须 `0x20000 build/xiaozhi.bin` 与
> `0x800000 build/generated_assets.bin` 一起写；只烧 app 会继续用旧阈值。

---

## 6. 验证记录

```text
09:38:38 MQTT: Connected to endpoint
09:38:38 StateMachine: State: activating -> idle
09:39:27 Custom wake word detected: command_id=1, string= ni hao shao yi, prob=0.244994
09:39:27 StateMachine: State: idle -> connecting
...
（长对话 / 多轮 TTS）
```

- 状态门控 AEC 后 10 分钟空闲 + 多轮对话：`task_wdt` **0 次**
- `Decode queue is full` 次数：0
- AEC 实际状态：`AEC on`（Listening）↔ `AEC off`（Idle，MultiNet 仍运行）
- 用户确认：闪烁消失，watchdog 消失，唤醒/TTS 正常

主机测试：

```sh
python3 -m unittest discover -s scripts/tests
# Ran 110 tests ... OK
```

---

## 7. 遗留风险与后续

1. **NLP `NORMAL` 的回声**：残留回声抑制比 `VERYAGGR` 弱；若长 TTS 时出现
   自打断/回声，回退到 `AGGR`，再观察 CPU/看门狗。
2. **阈值 0.15 的误唤醒**：若安静环境频繁误唤醒，回退到 18，同时保留
   `audio_afe` 绑核和 LVGL 锁修复。
3. **任务优先级**：`audio_output`/`opus_codec` 为 5，高于 LVGL 4 和
   `audio_afe` 3；正常解码/播放是短突发。若 UI 明显卡顿，可把两者降到 4
   并观察 TTS；不建议再降到 2。
4. **看门狗**：状态门控 AEC 后 10 分钟空闲 **0 次**；AEC+MultiNet 仍接近满载，
   但空闲不再跑无用的 AEC。若以后在高负载场景回归，优先考虑 16kHz、服务端 AEC
   或更轻的 WakeNet 模型，而不是继续调绑核。
5. **AEC 尾音**：状态回 Idle 时 AEC 立即关；若 `tts stop` 后还有 ~120ms 尾音，
   理论上可能进麦克风。当前实测无自唤醒；若出现，加 500ms 尾音保持或改播放时间戳门控。
6. **LVGL 并发**：本次只修了 `CustomLcdDisplay` 的两个入口；新增任何在 main
   任务里直接操作 LVGL 对象的代码，都必须用 `DisplayLockGuard`。

---

## 8. 快速烧录

```sh
# app + assets（唤醒阈值/唤醒词在 assets 里）
esptool.py --chip esp32s3 -p /dev/cu.usbmodem5CF70418411 -b 921600 \
  write_flash 0x20000 build/xiaozhi.bin \
              0x800000 build/generated_assets.bin
```

或全量：

```sh
esptool.py --chip esp32s3 -p /dev/cu.usbmodem5CF70418411 -b 921600 \
  write_flash 0x0 build/merged-binary.bin
```
