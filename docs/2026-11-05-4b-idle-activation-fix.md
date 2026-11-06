# 2026-11-05：4B 空闲态激活修复（双击/三击/唤醒词/IMU）

> 目标板型：`waveshare/esp32-s3-touch-lcd-4b`（ESP32-S3R8，16MB Flash / 8MB PSRAM，480×480 RGB，GT911，QMI8658）
>
> 固件版本：`2.5.2.1`；IDF：v6.1
>
> 真机串口：`/dev/cu.usbmodem5CF70418411`
>
> **2026-11-06 后续**：又修复了 TTS 解码饿死、唤醒阈值/绑核、AEC NLP 档位，以及
> `SetEmotion`/`SetTheme` 未持 LVGL 锁导致的“连接中卡死 + logo 周期闪烁”。后续记录见
> [2026-11-06-4b-audio-flicker-lock-fix.md](2026-11-06-4b-audio-flicker-lock-fix.md)。
> 本文中的 AEC NLP `VERYAGGR`、唤醒阈值 20、`opus_codec` 优先级 2 等描述已被后续
> 文档取代。

## 1. 结论

空闲态“双击不进聆听、三击不换主题、唤醒词不响应、IMU 不动”不是单一手势 bug，而是三层问题叠加：

1. **音频侧 CPU 饥饿（主因）**：`AEC_MODE_VOIP_LOW_COST` 让 `afe_fetch` 忙循环，`audio_input`/`audio_afe` 吃满双核，任务看门狗每 10~20s 触发；LVGL 和 IMU 任务被饿死。
2. **LVGL 调度问题**：LVGL 任务优先级只有 1、未绑核、触摸 50ms 轮询；空闲静态界面下短按可能完全采不到，500ms 双击确认定时器实测被延迟到 13s。
3. **功能缺失/检测方式不对**：代码里没有“空闲双击进聆听”；IMU 只比较加速度模长，拿起/翻转设备时 `|a|` 仍约 1g，永远不触发。

修复后真机验证（2026-11-05 17:57~18:03）：

| 验收项 | 结果 | 关键日志 |
|---|---|---|
| 说“你好，邵怡”唤醒 | ✅ | `Wake word detected: 你好，邵怡 (state: 3)` |
| 空闲双击进聆听 | ✅ | `Double-click: entering listening from idle` → `idle -> connecting -> listening` |
| 空闲三击换主题 | ✅ | `Switching theme: light -> dark` / `dark -> light` |
| IMU 晃动进聆听 | ✅ | `Motion detected ... delta=0.61g → StartListening` |
| 完整对话 | ✅ | `listening -> speaking -> listening`，TTS 正常 |

主机测试：`python3 -m unittest discover -s scripts/tests` → **110 tests OK**。
最终 4B 构建：`xiaozhi.bin` = `0x2db050`（2,994,256 B），app 分区余量约 27%。

---

## 2. 问题现象与定位过程

### 2.1 初始现象

用户报告：

- 空闲态双击屏幕无法进入“聆听中”
- 空闲态三击无法切换主题
- 按 BOOT 键进入“聆听中”后，三击可以正常切主题
- 空闲态 IMU 运动无法进入“聆听中”
- 期望：空闲态 **双击屏幕 / 说“你好，小威”（后改为“你好，邵怡”）/ 按 BOOT 键** 都能进入“聆听中”

### 2.2 串口证据：触摸事件被延迟 3.7 秒

第一次抓串口（旧固件）时看到：

```text
TouchDrv: cnt=1 x=349 y=354
TouchTrace: PRESS hit=container tick=31000
...（3.67 秒后）...
CustomLcdDisplay: CLICK target=other streak=1 state=3
```

`PRESS` 到 `SHORT_CLICKED` 间隔 **3.67s**，远大于 500ms 双击窗口。同时串口出现：

```text
task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
 - IDLE0 (CPU 0)
 - IDLE1 (CPU 1)
Tasks currently running:
 CPU 0: audio_input
 CPU 1: audio_afe
```

说明不是触摸驱动丢点，而是 **LVGL 任务拿不到 CPU/被阻塞**，点击事件在数秒后才被处理。

### 2.3 根因 1：AFE/AEC LOW_COST 忙循环

`main/audio/engines/afe_audio_engine.cc` 在 12:30 的提交里为了省 ~27KB 内部 SRAM，把已验证的组合改成了：

```cpp
afe_config_init(..., AFE_TYPE_VC, AFE_MODE_LOW_COST);
afe_config->aec_mode = AEC_MODE_VOIP_LOW_COST;
```

而该文件上方的注释已经写明：LOW_COST 会让 `afe_fetch` 持续失败、`audio_afe` 忙循环、看门狗每 10 秒触发。实测组合矩阵：

| AFE 模式 | AEC 模式 | 结果 |
|---|---|---|
| `AFE_MODE_LOW_COST` | `AEC_MODE_VOIP_LOW_COST` | `afe_fetch` 忙循环，双核吃满，看门狗 |
| `AFE_MODE_HIGH_PERF` | `AEC_MODE_VOIP_LOW_COST` | 仍然忙循环（17:15 日志） |
| `AFE_MODE_HIGH_PERF` | `AEC_MODE_VOIP_HIGH_PERF` | 不忙循环，但 AEC DSP 接近占满 core 0，MultiNet 占满 core 1；空闲手势/唤醒词仍失效 |
| **`AFE_MODE_LOW_COST`** | **`AEC_MODE_VOIP_HIGH_PERF`** | **✅ 最终采用**：不忙循环，整体 DSP 负载下降，给 MultiNet/LVGL/IMU 留出 CPU |

`audio_input` 的看门狗 backtrace 解码后确认热点在 AEC：

```text
afe_feed -> aec_process -> esp_voip_process_api
  -> dios_ssp_aec_process_api -> dios_ssp_aec_res_process
```

也就是说 AEC DSP 实际在 `audio_input`（core 0）里执行，而不是只在 `audio_afe` 里。

### 2.4 根因 2：LVGL 优先级/绑核/轮询周期

`RgbLcdDisplay` 原来：

```cpp
port_cfg.task_priority = 1;
port_cfg.timer_period_ms = 50;
```

问题：

- LVGL 优先级 1，低于 `audio_afe`（3）、`audio_input`（8）、主任务（10）；音频任务忙时 LVGL 定时器被延迟到十几秒。
- LVGL 未绑核，可能落在被 AEC 占满的 core 0。
- GT911 的 INT 脚没有接到 GPIO（`int_gpio_num = GPIO_NUM_NC`），触摸只能靠 LVGL 任务轮询；50ms 一次时，短于 50ms 的点击可能完全采不到。聆听中界面有动画、任务跑得更勤，所以“进聆听后三击反而正常”。

修复：

```cpp
port_cfg.timer_period_ms = 10;       // 100Hz 触摸轮询
port_cfg.task_affinity = 1;          // 固定 core 1
port_cfg.task_priority = 4;          // 高于 audio_afe(3)
```

### 2.5 根因 3：手势逻辑缺失与 IMU 检测方式

- `CustomLcdDisplay` 原来只有“双击清预览图/停播报、三击切主题”，**没有空闲双击进聆听**。
- `ImuMonitor` 原来用 `delta = fabsf(|a| - last_mag)`：
  - 设备平放、拿起、立起来时重力模长始终约 1g，`delta ≈ 0`；
  - 只有明显上下平移才触发，旋转/翻转检测不到。

---

## 3. 代码改动

### 3.1 音频：AFE 低配 + AEC 高配

文件：[main/audio/engines/afe_audio_engine.cc](../main/audio/engines/afe_audio_engine.cc)

```cpp
afe_config_t* afe_config =
    afe_config_init(input_format.c_str(), models_, AFE_TYPE_VC, AFE_MODE_LOW_COST);
...
afe_config->aec_init = codec_->input_reference();
afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
```

要点：

- **AEC 必须 HIGH_PERF**：这是 `afe_fetch` 不忙循环的关键；
- **AFE 用 LOW_COST** 降低整体 DSP 负载；
- 不要单独把 AEC 改回 LOW_COST，否则忙循环/看门狗回归。

### 3.2 内部 RAM：audio_input / opus_codec 栈移到 PSRAM

文件：[main/audio/audio_service.cc](../main/audio/audio_service.cc)、[main/audio/audio_service.h](../main/audio/audio_service.h)

AEC 高配会把内部 SRAM 吃到只剩几 KB（实测 `After AFE create: free=111`），语音会话会 ENOMEM。处理：

- `audio_input` 任务栈 6KB → PSRAM
- `opus_codec` 任务栈 24KB → PSRAM
- `StaticTask_t` 仍放内部 RAM
- 分配失败自动回退到原来的内部 RAM task

实现使用 `xTaskCreateStaticPinnedToCore` / `xTaskCreateStatic`，与工程里已有的 `audio_afe` PSRAM 任务栈做法一致。PSRAM 任务栈不用于 ISR，安全。

效果：空闲内部 SRAM 从约 4.4KB 提升到约 20.5KB，语音会话可正常进行。

### 3.3 LVGL 调度与触摸轮询

文件：[main/display/lcd_display.cc](../main/display/lcd_display.cc)

```cpp
lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
port_cfg.task_priority = 1;
port_cfg.timer_period_ms = 10;       // 原 50
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;      // 固定 core 1
    port_cfg.task_priority = 4;      // 原 1
#endif
```

### 3.4 手势：双击进聆听，三击换主题

文件：[main/boards/waveshare/esp32-s3-touch-lcd-4b/custom_lcd_display.cc](../main/boards/waveshare/esp32-s3-touch-lcd-4b/custom_lcd_display.cc)、[.h](../main/boards/waveshare/esp32-s3-touch-lcd-4b/custom_lcd_display.h)

点击判定只注册一次，挂在屏幕根节点，子对象全部开 `LV_OBJ_FLAG_EVENT_BUBBLE`，避免 container_ / preview_viewport_ 重复注册导致一次点击计两次。

双击动作**不立即执行**，先等 500ms 确认窗口；窗口内来第三击就改判为三击切主题：

```text
双击优先级（500ms 确认后执行）：
  1. Notifying（播报中）      -> 停止播报（notify_stopped=double_click）
  2. 有常驻预览图             -> 清除该预览图（image_cleared=double_click）
  3. Idle（空闲）             -> StartListening(GetDefaultListeningMode())
  4. 其它状态                 -> 无动作
三击：
  - 取消待执行的双击动作
  - ToggleTheme()（light <-> dark），并上报 theme_changed
```

细节：

- 双击只在 Idle 才进聆听，不会在 Connecting/Listening/Speaking 中误触发；
- 进聆听走 `Application::Schedule()` + 主任务，和 IMU 触发路径一致，避免在 LVGL 任务里读主任务字段；
- 三击到达时暂停确认定时器，避免“先清图/先进聆听再切主题”；
- 新序列开始前若上一条双击还没执行，先补执行再取消，避免 LVGL 繁忙时双击被吞。

### 3.5 IMU：三轴矢量差

文件：[main/boards/waveshare/esp32-s3-touch-lcd-4b/imu_monitor.h](../main/boards/waveshare/esp32-s3-touch-lcd-4b/imu_monitor.h)

```cpp
const float dx = x - last_x_;
const float dy = y - last_y_;
const float dz = z - last_z_;
const float delta = sqrtf(dx * dx + dy * dy + dz * dz);
```

- 采样率 5Hz → 10Hz（100ms）
- 阈值 `MOTION_THRESHOLD = 0.20f`（三轴矢量差，g）
- 首帧只建立基线，不触发
- 保留 5s 冷却和点击/按键后 700ms 抑制
- 仍只在 Idle / Notifying 触发

### 3.6 唤醒词：你好，邵怡

文件：[main/boards/waveshare/esp32-s3-touch-lcd-4b/config.json](../main/boards/waveshare/esp32-s3-touch-lcd-4b/config.json)

```json
"CONFIG_CUSTOM_WAKE_WORD=\"ni hao shao yi\"",
"CONFIG_CUSTOM_WAKE_WORD_DISPLAY=\"你好，邵怡\"",
```

真机加载日志：

```text
CustomWakeWord: Command: ni hao shao yi, Text: 你好，邵怡, Action: wake
...
CustomWakeWord: Custom wake word detected: command_id=1,
                string= ni hao shao yi, prob=0.205268
Application: Wake word detected: 你好，邵怡 (state: 3)
```

> **坑：唤醒词命令在 assets 分区，不在 app 里。**
> 第一次只改了 `config.json` 并只烧 app，运行时仍然打印 `ni hao xiao wei`。原因：
> `CustomWakeWord::Initialize()` 在 `models_list != nullptr` 时走 `ParseWakenetModelConfig()`，
> 从 assets 的 `index.json` 读取 `multinet_model.commands`。
> 必须同时烧 `build/generated_assets.bin` 到 **0x800000**，才会加载新词。

### 3.7 清理

- `esp32-s3-touch-lcd-4b.cc`：恢复被诊断改动抑制的 `OnEnterSleepMode`（`SetPowerSaveMode(true)` + 背光 70）。
- 本机 `managed_components/espressif__esp_lvgl_port/.../esp_lvgl_port_touch.c`：去掉每个触摸点都打印的 `TouchDrv` 日志，并去掉一次冗余 `esp_lcd_touch_get_data()` 调用。**该文件不在 git 跟踪内**，重新拉取 managed_components 会恢复原样；它只影响串口噪声/触摸轮询开销，不影响功能。
- `PROJECT_OVERVIEW.md`、`docs/flashing.md`：同步 4B 新唤醒词、空闲双击进聆听、assets 烧录注意事项。

---

## 4. 构建与烧录

### 4.1 构建

```sh
source ~/.espressif/tools/activate_idf_v6.1.sh
python3 scripts/build.py waveshare/esp32-s3-touch-lcd-4b --name esp32-s3-touch-lcd-4b
```

产物：

- `build/xiaozhi.bin`：app，烧 `0x20000`
- `build/generated_assets.bin`：资源，烧 `0x800000`
- `build/merged-binary.bin`：完整合并镜像，从 `0x0` 烧

### 4.2 只更新 app

```sh
esptool.py --chip esp32s3 -p /dev/cu.usbmodem5CF70418411 -b 921600 \
  write_flash 0x20000 build/xiaozhi.bin
```

### 4.3 更新唤醒词（必须 app + assets）

```sh
esptool.py --chip esp32s3 -p /dev/cu.usbmodem5CF70418411 -b 921600 \
  write_flash 0x20000 build/xiaozhi.bin \
              0x800000 build/generated_assets.bin
```

或全量：

```sh
esptool.py --chip esp32s3 -p /dev/cu.usbmodem5CF70418411 -b 921600 \
  write_flash 0x0 build/merged-binary.bin
```

### 4.4 主机测试

```sh
python3 -m unittest discover -s scripts/tests -v
# Ran 110 tests ... OK
```

---

## 5. 真机验证记录

### 5.1 旧固件（问题复现）

- `task_wdt` 每 10~20s 触发，`CPU 0: audio_input` / `CPU 1: audio_afe`
- 触摸 `PRESS -> SHORT_CLICKED` 3.67s
- 空闲点击后双击动作 13s 才执行
- 画面在聆听中三击可换主题，空闲不行（LVGL 在聆听中反而有更多调度机会）

### 5.2 最终固件（通过）

```text
18:02:48 CustomWakeWord: Command: ni hao shao yi, Text: 你好，邵怡, Action: wake
18:03:07 CustomWakeWord: Custom wake word detected: command_id=1,
         string= ni hao shao yi, prob=0.205268
18:03:07 Application: Wake word detected: 你好，邵怡 (state: 3)
18:03:07 StateMachine: State: idle -> connecting
18:03:07 StateMachine: State: connecting -> listening
18:03:07 StateMachine: State: listening -> speaking
18:03:09 Application: << 你好呀！
18:03:10 Application: << 我是小威，有什么可以帮你的吗？
```

手势：

```text
17:59:58 CustomLcdDisplay: Double-click: entering listening from idle
17:59:58 StateMachine: State: idle -> connecting
17:59:58 StateMachine: State: connecting -> listening

18:00:01 CustomLcdDisplay: Switching theme: light -> dark
18:00:02 CustomLcdDisplay: Switching theme: dark -> light
```

IMU：

```text
ImuMonitor: Motion detected: x=-0.14g y=0.43g z=-0.85g delta=0.61g → StartListening
StateMachine: State: idle -> connecting
StateMachine: State: connecting -> listening
```

---

## 6. 坑与注意事项

### 6.1 唤醒词只烧 app 不生效

见 §3.6。`index.json` 在 assets 分区，必须烧 `0x800000`。只改 `config.json`/`sdkconfig` 并烧 app，运行日志会继续显示旧词。

### 6.2 `sdkconfig` 与 `config.json` 的 defaults 关系

`config.json` 里的 `sdkconfig_append` 写入 `build/xiaozhi-build.sdkconfig.defaults`，再作为 `SDKCONFIG_DEFAULTS` 参与配置；**已有 `sdkconfig` 的值优先级更高**。如果切板/改词后 `sdkconfig` 里仍是旧值，需要重新生成或在干净配置下构建。

### 6.3 不要用 `idf.py build` 混用 Python 环境

本机曾出现：

```text
'/.../python' is currently active ... configured with '/.../python3'
Run 'idf.py fullclean' to start again.
```

统一用 `scripts/build.py`，它会处理 `python`/`python3` 的兼容。

### 6.4 `build/log` 权限问题

`idf.py` 重配置时偶尔因 `build/log` 的 `EEXIST`/权限失败；可把旧 `build/log` 移走，或 `cd build && ninja all`（先 reconfigure）。

### 6.5 厂商组件的本地改动不可复现

`managed_components/` 是生成目录，不在 git 跟踪内。本次去掉了 `TouchDrv` 日志和冗余 `get_data`，重新拉取组件后会恢复。若要长期保留，应通过 patch/上游方式，而不是依赖本地目录。

### 6.6 `clang-format` 与既有文件

`custom_lcd_display.cc/.h`、`esp32-s3-touch-lcd-4b.cc`、`imu_monitor.h` 等文件在本次修改前就存在 clang-format 违规（include 顺序、宏对齐等）。为避免无关 mass formatting，本次没有整文件格式化，只保证新增代码风格与上下文一致。

### 6.7 任务看门狗仍会打印

最终固件空闲/对话时仍可能看到：

```text
task_wdt: - IDLE0 (CPU 0)
task_wdt: Tasks currently running:
task_wdt: CPU 0: audio_input
task_wdt: CPU 1: audio_afe
```

原因：AEC 高配 + MultiNet 在 S3 上接近满载。当前配置下：

- 不会 panic/复位（`CONFIG_ESP_TASK_WDT_PANIC` 未开启）
- 不再阻塞 LVGL/IMU（LVGL 已绑 core 1、优先级 4）
- 手势/唤醒/对话均正常

若要彻底消除，可选（按代价从低到高）：

1. `AEC_NLP_LEVEL_VERYAGGR` → `AEC_NLP_LEVEL_AGGR/NORMAL`（省 CPU，回声抑制略弱）
2. 改服务端 AEC（`CONFIG_USE_SERVER_AEC=y`，`aec_init=false`），设备端省掉 AEC DSP，但失去本地回声消除
3. 用更轻的 WakeNet 自定义模型替代 MultiNet（需要单独的唤醒词模型）
4. 关掉 `CONFIG_SPIRAM_FETCH_INSTRUCTIONS/RODATA`（提升代码执行速度，但可能重新引入 RGB 闪屏）

### 6.8 3.49 测试板仍是“你好，小威”

`main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/config.json` 仍是旧唤醒词，作为测试基线未改动。若也要改，需要同步重烧 3.49 的 app + assets。

### 6.9 设备自称“小威”不是固件问题

设备 `hello` 消息只发送 `type/version/features/audio_params`，**不发送助手名字**。对话里的“我是小威”来自：

- 小智控制台里绑定该设备的**智能体人设/系统提示词**；或
- 自建服务端的 agent prompt / system prompt

固件侧只能改唤醒词，改不了服务端自称。需要到控制台把“你叫小威”改成“你叫邵怡”（或对应 agent 名称）。

---

## 7. 涉及文件

| 文件 | 改动 |
|---|---|
| `main/audio/engines/afe_audio_engine.cc` | AFE 低配 + AEC 高配；记录组合矩阵与回归原因 |
| `main/audio/audio_service.cc` / `.h` | `audio_input`/`opus_codec` 栈移 PSRAM，失败回退 |
| `main/display/lcd_display.cc` | RGB LVGL 任务 core 1 / 优先级 4 / 触摸 10ms |
| `main/boards/waveshare/esp32-s3-touch-lcd-4b/custom_lcd_display.cc` / `.h` | 双击进聆听 + 三击换主题；500ms 确认窗口；屏幕根节点单点注册 |
| `main/boards/waveshare/esp32-s3-touch-lcd-4b/imu_monitor.h` | 三轴矢量差、10Hz、0.20g |
| `main/boards/waveshare/esp32-s3-touch-lcd-4b/esp32-s3-touch-lcd-4b.cc` | 恢复省电路径 |
| `main/boards/waveshare/esp32-s3-touch-lcd-4b/config.json` | 唤醒词“你好，邵怡” |
| `PROJECT_OVERVIEW.md` / `docs/flashing.md` | 同步 4B 行为与烧录说明 |

---

## 8. 后续待办

- [ ] 服务端智能体提示词改名“小威”→“邵怡”
- [ ] 若要求零 `task_wdt`，按 §6.7 选一项优化并复测
- [ ] 3.49 测试板如需统一唤醒词，重烧其 app + assets
- [ ] 将 `managed_components` 的触摸日志清理做成可复现 patch（可选）
- [ ] 在稳定网络下补一次 10 分钟连续对话/播报回归（本次已验证连接、聆听、说话、唤醒打断）
