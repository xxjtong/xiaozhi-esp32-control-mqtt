# 威思客定制版 → 上游 2.5.2 迁移准备：本地定制全量盘点与冲突评估

> 基线：上游 `78/xiaozhi-esp32` **v2.4.2**（本 fork 的上游基准）
> 目标：上游 **v2.5.2**
> 当前 fork：`2.4.2.1`（`PROJECT_VER`，工作区含未提交改动，本报告按**工作区实际内容**统计，而非 git HEAD）
> 生成日期：2026-11-03

---

## 0. 一页结论

| 指标 | 数值 |
|---|---|
| 相对上游 2.4.2 有差异的文件 | **75** 个（另加 46 条新增文件/目录） |
| 其中已是 2.5.2 代码（此前已前向移植） | **24** 个 |
| 真正携带本地定制的文件 | **27** 个，本地新增约 **1,880** 行 |
| 纯 fork 新增文件 | **约 4,300** 行（含 logo 位图 1,100 行；业务逻辑约 3,200 行） |
| 三路合并演练冲突 | **17** 个文件 / **29** 个冲突块 |
| 上游未改动、可直接整文件保留 | **19** 个文件 |

三条判断：

1. **迁移难度中等偏低，可控。** 冲突块只有 29 个，其中约一半是格式/注释级差异（clang-format 重排、注释措辞），真正需要语义判断的约 10 处。
2. **真正的风险不在合并，而在依赖升级。** 2.5.2 把 `idf: '>=5.5.2'` 抬到 **`>=6.0.1`**，并把 `78/esp-ml307` 从 3.6.6 升到 **3.7.5**。后者把 `Http` 接口从 `bool/int` 改成 `std::expected`，**直接打断 fork 自研的 `notify/notify_player.cc`**（4 个冲突块全部由此产生）。
3. **本次迁移有 3 处"静默回退"陷阱必须人工确认**——它们不会报冲突，但会被上游代码"悄悄修回去"：
   - `Application::CheckNewVersion()` 中**自动 OTA 升级块**（fork 已删除，2.5.2 仍保留，靠三路合并的删除语义保住，必须回归验证）；
   - `main/Kconfig.projbuild` 的 `ESP_VIDEO_USE_CUSTOMIZED_ESP_H264_VERSION` 占位符（若被清掉，依赖解析会直接失败）；
   - `main/CMakeLists.txt` 的 `notify/notify_player.cc` / `control_mqtt.cc` 两个源文件条目（丢掉就是链接期报错，尚可发现）。

---

## 1. 基线与方法

本地仓库**没有上游历史**——首个提交 `545e9bd "Initial import of xiaozhi-esp32 with second MQTT control channel"` 就是把上游代码 + 定制一起倒进来的，所以 `git diff` 对本任务无效。

本次采用的方法是：

1. 从 `codeload.github.com` 拉取 `v2.4.2` 与 `v2.5.2` 的 tag 归档（不走 `git clone`，本机对 GitHub 的 git 协议不通但 HTTPS 正常）；
2. 用 `rsync` 生成本地工作区快照，排除 `build/`、`build_4b/`、`managed_components/`、`dist/`、`tmp/`、`sdkconfig`、`.git` 等生成物；
3. `diff -rq` 得到 75 个差异文件，再看哪些文件与 2.5.2 逐字节相同——**这一步把"定制"和"已移植"分离开，工作量估算因此从 75 个文件降到 27 个**；
4. 在 `/tmp/cmp/merge` 建了一个三支仓库（`base = 2.4.2` / `upstream252` / `fork = 2.4.2.1`）做真实 `git merge`，拿到确切冲突面。

> 复现命令见 `file-matrix.csv` 同目录，如需重跑可参考第 7 节。

---

## 2. 定制总体分布

按功能域归类，本地定制的重心非常集中：

| 功能域 | 涉及文件 | 本地新增行 | 迁移冲突 |
|---|---|---|---|
| 第二路控制 MQTT（控制平面） | `main/control_mqtt.{cc,h}` + 6 处挂钩 | 1,432 + 挂钩 | 挂钩点随 application.cc 冲突 |
| 远程语音播报 closed loop | `main/notify/*`、`application.cc`、`control_mqtt.cc` | 347 + 挂钩 | **4 块（Http API）** |
| 显示层（壁纸/预览/手势/主题/时钟） | `lcd_display`、`lvgl_display`、`custom_lcd_display`×2、`oled_display` | ~800 | 2 块（`lvgl_display.h`、`oled_display.h`） |
| 板级硬件外设（IMU/RTC/电池/背光守护/显示使能守护） | 两个 Waveshare 板 + `board.h`/`backlight`/`wifi_board` | ~1,200 | 2 块（`esp_video.cc`、`otto_robot.cc`） |
| 协议层健壮性 | `mqtt_protocol`、`websocket_protocol`、`ota`、`settings` | ~70 | 5 块（`mqtt_protocol` 3 块上游已吸收） |
| 构建与工具链 | `scripts/build.py`、`test_build.py`、`Kconfig`、`CMakeLists` | ~300 | 2 块 |
| 文档 | `docs/control-mqtt*.md`、`flashing.md`、发布记录等 14 份 | — | 1 块（`notify.md`） |

---

## 3. 定制功能清单

### 3.1 第二路控制 MQTT（`main/control_mqtt.{cc,h}`，1,432 行）

fork 的核心能力：与小智云主协议**完全独立**的第二条 MQTT 链路，只做控制平面，不走 UDP 音频、不占会话。

- **连接管理**：`Start()` 非阻塞，阻塞式 connect 放在独立任务里；后续依赖 esp-mqtt 自动重连，`OnConnected` 里重新订阅。`Application::HandleNetworkDisconnectedEvent()` **刻意不拆这条链路**（拆了就等到重启）。
- **配置**：`NVS namespace "control_mqtt"`（`endpoint` / `username` / `password`）优先于 `Kconfig` 默认值 `mqtt.virsical.cn:21883`（8883 走 TLS，其余明文）。
- **主题**：下行 `xiaozhi/{device_id}/cmd`，上行 `xiaozhi/{device_id}/reply` + `hello`/`goodbye`/`session_update`/`notify_result`/`event`。
- **15 个指令处理器**：`listen`、`abort`、`mcp`、`system`、`set_image`、`set_theme`、`set_volume`、`set_brightness`、`toggle_wake_word`、`notify`、`abort_notify`、`set_status`、`toggle_power_save`、`set_imu`、`query_status`。
- **线程模型**：所有设备状态变更一律 `Application::Schedule()` 回主任务；图片下载单飞（`image_fetch_running_` 原子量），解码结果回主任务应用。
- **挂钩点**（分布在核心文件，是迁移时最容易漏的部分）：

| 挂钩位置 | 作用 |
|---|---|
| `Application::InitializeProtocol()` 末尾 | `control_mqtt_ = make_unique<ControlMqtt>(); Start();` |
| `Application::OnStateChanged()` 末尾 | `BroadcastState(DeviceStateToString(new_state))` |
| `Application::ReportUserEvent()`（新增方法） | 本地按键/触摸/唤醒词/IMU 事件上报，QoS 0 尽力而为 |
| `Application::HandleIncomingJson()` | 解析主协议的 `notify` 消息（带字幕） |
| `Application::StartNotification/StopNotification/StopNotificationByUser/HandleNotificationFinished` | 播报状态机 |

### 3.2 远程语音播报 closed loop

fork 在 2.5.2 的 `notify` 基础上补了**送达确认**——这是与上游最大的语义分叉：

| 增量 | 上游 2.5.2 | 本 fork |
|---|---|---|
| `notify.id` 对账 | 无 | 有，回执与 `notify_result` 都带回 |
| 上行 `notify_result` | 无（服务端只能靠 `duration_ms` 猜） | 有，`outcome ∈ {finished, cancelled, user_stopped, aborted, interrupted}` + `duration_ms` |
| 下行 `abort_notify` | 无 | 有 |
| 本地停止手势 | 无 | 屏幕双击 / BOOT 键 / IMU 姿态变化，且 `outcome=user_stopped` |
| 新状态 `kDeviceStateNotifying` | **有**（2.5.2 已有） | 有，并扩展了 17 处状态分支 |

> 注意：`kDeviceStateNotifying` 与 `notify/notify_player.cc` 本身就是**从 2.5.2 反移植回来的**，所以这部分不是"要重新移植"，而是要"把 fork 在上面的增量重新贴回去"。

### 3.3 显示层

- **远程图片（`set_image`）**：临时预览支持**上下拖动**（`preview_viewport_` 滚动视口，替代上游固定 `width_/2 × height_/2`）；缩放规则改为"占屏宽 80%、不放大超过原图"，长图从顶部开始。
- **壁纸（`set_image mode=background`）**：新增 `CreateCoverBackground()`——cover 缩放 + 居中裁切，直接生成 RGB565 全屏缓冲；**自研 PNG 解码路径**（`lv_image_decoder_open` → ARGB8888 → 手写合成到 RGB565），因为 LVGL 懒解码在锁内不可控。同时装到 light/dark 两个主题，`HasBackgroundImage()` 供 `query_status` 查询。
- **手势**：双击清预览图 / 停播报，三击切主题。**不用** `LV_EVENT_DOUBLE_CLICKED`——LVGL 9 把单/双/三击做成 3 次循环下发，这里改用 `LV_EVENT_SHORT_CLICKED` + 500 ms 窗口自行计数，并支持"窗口内来了新图则不误清"。
- **公司 logo**：开机 `robot_2` 与空闲 `neutral` 两个时机用全屏 logo 顶替机器人图标，深/浅主题各一版。
- **状态栏**：`SetStatusColor()` 支持远程改色（下次 `SetStatus` 自动回主题色）；时钟改为"分钟变化才刷新"；电量显示从纯图标改为`图标 + NN%`。
- **OLED**：`swap_xy` 形参 + `SetupUI_128x32_Portrait()` 竖屏布局（物理 128×32 旋转 90° → 逻辑 32×128）。
- **渲染健壮性（重要）**：`custom_lcd_display.cc` 把 `portMAX_DELAY` 改成 500 ms 超时，失败也调 `lv_disp_flush_ready()`，并在持续失败 20 s 后 `Schedule(Reboot)` 自恢复。修的是"黑屏但还能说话、串口静默"的死锁。

### 3.4 板级硬件外设（Waveshare 3.49-V2 / 4B）

| 能力 | 实现 | 说明 |
|---|---|---|
| 背光守护 | `Backlight::ReapplyBrightness()`（`PwmBacklight` 回读 LEDC 占空比比对） | 已在**基类**里加虚函数，属于 core-common 改动 |
| 显示使能守护 | 3.49-V2 的 `DisplayEnableGuard` 低优先级任务，1 s 周期回读 TCA9554 EXIO1/EXIO5 | 修 TCA9554 POR 后背光永久丢失 |
| IMU | `imu_monitor.h`（QMI8658 @0x6B，音频 I2C 0x47/0x48） | 姿态变化→触发聆听；`NotifyUserInteraction()` 抑制点击/按键抖动误触发；可经 `set_imu` / NVS `imu/enabled` 关闭 |
| RTC | `rtc_monitor.h`（PCF85063 @0x51） | 与 SNTP 共用，`RTC_TIMEZONE_OFFSET_SEC = 8h` |
| 电池 | 3.49-V2 新增 `AdcBatteryMonitor`（ADC1_CH3/GPIO4，200k:100k） | 先取电量再读充电态（估算器内部推进） |
| BOOT 8 s 长按 | 清 `mqtt`/`websocket` 两个 NVS 命名空间并重启 | 配置失效时的现场恢复手段 |
| 网络信息 | `Board::GetIpAddress/GetWifiSsid/GetWifiRssi` 新虚接口 | `WifiBoard`/`EthernetBoard`/`DualNetworkBoard` 均已实现 |
| 4B 板 | `CustomLcdDisplay : RgbLcdDisplay`（复用上游 RGB 刷新管线，只叠手势 + logo） | 4B 未走 3.49-V2 那套手写 flush |

### 3.5 系统与协议层健壮性

| 文件 | 改动 | 迁移处置 |
|---|---|---|
| `main/ota.cc` | `ParseVersion` 改 `from_chars`，非法版本返回空而非抛异常 | **采用上游**——2.5.2 已用 `std::expected` 做了等价甚至更完整的实现 |
| `main/protocols/mqtt_protocol.cc/h` | `unique_ptr<Udp>` → `shared_ptr<Udp>`，`Send()` 移出 `channel_mutex_` | **采用上游**——上游注释写着同样的理由，重构已吸收 |
| `main/application.cc` | 版本检查失败时 `snprintf` 截断保护；`CheckNewVersion` 前**跳过激活流程**（NVS 已有协议配置时直连） | 需手工合并（OTA 返回值改成了 `expected`） |
| `main/application.cc` | **删除自动 OTA 升级块**（`if (ota_->HasNewVersion()) UpgradeFirmware(...)`） | **必须保留**，见第 0 节陷阱 |
| `main/protocols/websocket_protocol.cc` | `transport` 字段 `cJSON_IsString` 校验 | 保留 |
| `main/settings.cc` | `nvs_handle_ == 0` 时空句柄保护 | 保留 |
| `main/display/lvgl_display/lvgl_theme.cc` | `#fff` 短色值导致 `substr` 抛异常 → 加长度判断 | 取上游（同义） |
| `main/notify/notify_player.cc` | 播报播放进度回调 `on_playback_progress`（`AudioTask.playback_id/media_position_ms`） | 保留字段，重挂到 2.5.2 的值语义 `AudioTask` |

### 3.6 构建与工具链

- `scripts/build.py`：① 优先用 `$IDF_PATH/tools/idf.py` 直调（不依赖 PATH 里的 `idf.py`）；② 新增 `_board_supports_wake_word()`，按 target + PSRAM 判定变体是否支持唤醒词；③ `=n` 校验放宽——Kconfig 因依赖不满足而**整符号消失**时视作"未启用"，避免误报。
- `scripts/tests/test_build.py`：对应补 2 个用例（含 `esp-vocat` 样式覆盖）。
- `main/Kconfig.projbuild`：`CONTROL_MQTT_ENDPOINT/USERNAME/PASSWORD`、WK 板选项组、`LCD_NV3030B_240X320`、`ESP_VIDEO_USE_CUSTOMIZED_ESP_H264_VERSION` 占位符。
- `main/idf_component.yml`：尾部追加 `cube32esp/xpowerslib: ^0.3.3`（2.5.2 也有这一行，已对齐）。
- `sdkconfig.defaults`：删除 `CONFIG_NEWLIB_NANO_FORMAT`（picolibc 下不可见）与两个 `LV_USE_CALENDAR_HEADER_*`。

### 3.7 文档（14 份，全部 fork 独有）

`control-mqtt.md`（协议总览）、`control-mqtt-api.md`（指令表）、`control-mqtt-notify-feedback.md`（播报闭环 + 真机记录）、`tts-broadcast-design.md`、`flashing.md`、`virsical-release-2.4.2.1-2026-10-26.md`（测试台账）、板型对比 4B vs P4、桌面 AI 徽章架构/客户手册、`workbuddy-open-platform-assessment.md`、`xiaozhi-presentation.html/pdf`、`PROJECT_OVERVIEW.md`。

> 迁移后需同步更新的文档：`PROJECT_OVERVIEW.md`（版本与目录树）、`virsical-release-*.md`（新版本记录）、`control-mqtt*.md`（若指令有变）。

---

## 4. 已前向移植：24 个文件零工作量

以下文件此前已整体拉到 2.5.2 版本，**逐字节一致**，迁移时直接用 2.5.2 覆盖即可，不需要任何判断：

```
docker/firmware-builder/{Dockerfile,README.md,firmware_builder.py,test_firmware_builder.py}
main/audio/demuxer/ogg_demuxer.{cc,h}
main/audio/engines/lite_audio_engine.{cc,h}
main/boards/bread-compact-esp32/config.h
main/boards/bread-compact-nt26/{compact_nt26_board.cc,config.h}
main/boards/common/i2c_device.{cc,h}
main/boards/dfrobot/df-k10/led_control.cc
main/boards/jiuchuan-s3/jiuchuan_dev_board.cc
main/boards/lckfb/szpi-esp32s3/lichuang_dev_board.cc
main/boards/m5stack/stopwatch/m5stack_stopwatch.cc
main/boards/otto-robot/README.md
main/boards/waveshare/esp32-s3-touch-lcd-4.3c/config.json
main/device_state_machine.cc
main/led/{circular_strip.cc,circular_strip.h,gpio_led.cc,single_led.cc}
```

另外这些**整目录/整文件也是 2.5.2 原样**，属于"已随上游带入"：

```
main/boards/wk-esp32s3-dev/            （整块 WK 板，含 NV3030B/ILI9486 驱动）
main/boards/waveshare/esp32-c6-epaper-1.54/   （整块墨水屏板）
main/boards/common/xpower.h
main/cjson_utils.h
main/assets/locales/mr-IN/             （马拉地语，README "39 种语言"的来源）
```

### 4.1 已被上游 2.5.2 吸收的 3 处本地改动（可直接丢弃）

这几处当年是 fork 自己改的，2.5.2 上游做了等价甚至更好的实现——**迁移时不要试图保留本地版本**，否则是无意义的长期维护负担：

| 本地改动 | 上游 2.5.2 的等价实现 | 结论 |
|---|---|---|
| `ota.cc`：`ParseVersion` 改用 `from_chars` 防抛异常 | 改成 `std::expected`，并额外区分"无法解析当前版本"与"无法解析服务器版本" | 取上游 |
| `mqtt_protocol.cc/h`：`shared_ptr<Udp>` + `Send()` 移出锁 | 完全相同的重构，注释理由一字不差 | 取上游 |
| `esp_video.cc`：`V4L2_PIX_FMT_RGB565X` 纳入像素格式表 | 2.5.2 已自带同样两行 `case` | 取上游，本地只留 Capture 失败日志 |

---

## 5. 上游 2.5.2 的破坏性变化

迁移顺序必须服从这几条，否则会连环报错。

### 5.1 工具链与依赖（最高优先级）

| 项 | 2.4.2 | 2.5.2 | 影响 |
|---|---|---|---|
| `idf` 最低版本 | `>=5.5.2` | **`>=6.0.1`** | 本机已装 **v6.0.2**，满足；但 `78/uart-eth-modem` 在 fork 里被钉在 `==0.6.0`（注释说明为兼容 IDF 5.5.2），2.5.2 已升到 `~0.7.0`，需解锁 |
| `78/esp-ml307` | `~3.6.6`（实际装 3.6.7） | **`~3.7.5`** | **`Http` 接口改为 `std::expected`**：`Open()` 返回值、`Read()`、`GetStatusCode()` 全部换签名 → `notify_player.cc` 4 处编译失败 |
| `78/esp-wifi-connect` | `~3.2.2` | `~3.3.1` | 网络抽象层，`Network` 接口同步调整 |
| `esp_lvgl_port` | `~2.8.0` | `~2.9.0` | 显示刷新 API，`custom_lcd_display.cc` 需回归 |
| `esp_video` | `^2.3.0` | `^2.4.1` | `esp_video.cc` 上游重写 131 行 |
| `esp-sr` | `~2.4.7` | `==2.4.7`（钉死） | 唤醒词 Kconfig 依赖判定可能变化，影响 `build.py::_board_supports_wake_word()` |

### 5.2 代码层 API 变更（会造成编译错误，不是冲突）

| 变更 | 位置 | fork 受影响处 |
|---|---|---|
| `Ota::CheckVersion()` 返回 `std::expected`，错误对象有 `ToString()/Message()` | `ota.cc/h` | `application.cc` 版本检查错误提示（本地 `snprintf` 硬化逻辑） |
| `Ota::ParseVersion()` 返回 `std::expected<vector<int>, string>` | `ota.cc/h` | 本地已改的那版要整段丢弃 |
| `AudioTask` 由 `unique_ptr<AudioTask>` 改为**值语义**，队列换 `fixed_queue.h` | `audio_service.h/cc` | 本地 `playback_id` / `media_position_ms` 两行赋值要改成 `task.xxx` |
| `MqttProtocol::udp_` 改 `shared_ptr`，`Send()` 移到锁外 | `mqtt_protocol.*` | 与本地改动**同源**，直接取上游 |
| `LvglDisplay::SetPreviewImage` 补 `override` | `lvgl_display.h` | 与本地新增 5 个虚函数并存即可 |
| `main/display/` 新增 `epd_display/`、`lvgl_psram_pool.h`、`lvgl_no_psram_pool.h` | 新文件 | 本 fork 未适配，可先不启用，但 `Kconfig`/`CMakeLists` 会自动带入 |

### 5.3 上游新增能力（可选采纳）

新板：`chd`、`espressif/esp-mosaico`、`folotoy`、`guition`、`lc-s3-wifi-1.54tft`、`lilygo/t-circle-s3-v11`、`maza-ai`、`m5stack/tab5`（ST7121 驱动）。
新工具链：`.github/workflows/release.yml`、`scripts/release.py`、`scripts/ci/representative-variants.json`、4 个新测试。
删除：`docs/esp-idf-6-migration.md`。

---

## 6. 合并演练结果：17 文件 / 29 块

三支合并（`base=2.4.2`，`upstream=2.5.2`，`fork=2.4.2.1`）的实际冲突分布：

| 文件 | 冲突块 | 性质 | 处置 |
|---|---|---|---|
| `main/application.cc` | 5 | 2 块 OTA `expected` 适配 + 3 块 `StopNotification` vs `StopNotificationByUser` | 手工，保 fork 语义 |
| `main/notify/notify_player.cc` | 4 | 全部是 Http API `bool/int` → `expected` | 手工，取上游写法 |
| `main/audio/audio_service.cc` | 4 | 3 块 `AudioTask` 值语义 + 1 块格式 | 手工，取上游结构 |
| `main/ota.cc` | 2 | `ParseVersion` 重写 + 头文件 | **取上游**（本地意图已被覆盖） |
| `main/protocols/mqtt_protocol.cc` | 2 | 格式/初始化差异 | **取上游** |
| `main/boards/otto-robot/otto_robot.cc` | 1 | `#include <new>` | 取上游，另需重放摄像头检测拆分 |
| `main/boards/common/esp_video.cc` | 1 | 纯换行格式 | 取上游（RGB565X 支持上游 2.5.2 已自带，本地真正独有的只剩 Capture 失败日志） |
| `main/Kconfig.projbuild` | 1 | 新增 CHD 板选项，正好落在 WK 选项组后 | 两边都留 |
| `main/application.h` | 1 | `StartNotification` 声明位置 | 两边都留 |
| `main/display/lvgl_display/lvgl_display.h` | 1 | `override` 与 5 个新虚函数 | 两边都留 |
| `main/display/lvgl_display/lvgl_theme.cc` | 1 | 注释措辞 | 取上游 |
| `main/display/oled_display.h` | 1 | `swap_xy` 形参 | 保 fork |
| `main/idf_component.yml` | 1 | m5stack 版本号相邻 | 保 fork 的 xpowerslib + 上游版本 |
| `main/protocols/mqtt_protocol.h` | 1 | 上游补了设计注释 | 取上游 |
| `CMakeLists.txt` | 1 | `PROJECT_VER` | 改 `2.5.2.1` |
| `.github/workflows/build.yml` | 1 | 上游重写 CI 变更选择 | 取上游 |
| `docs/notify.md` | 1 | fork 追加的 "Fork extension" 段 | 保 fork |

**自动合并成功但必须回归验证**的文件（不冲突 ≠ 正确）：

| 文件 | 风险点 |
|---|---|
| `main/application.cc` | "跳过版本检查"整块、自动 OTA 删除、notify 字幕解析是否被上游新版覆盖 |
| `main/display/lcd_display.cc` | 上游 +32 行若动到 `SetupUI()`/`SetPreviewImage()`，`preview_viewport_`（16 处引用）可能半途失效 |
| `scripts/build.py` | 上游 +206/-51，本地 3 项改进可能"语法上还在、逻辑上被绕过" |
| `main/CMakeLists.txt` | 上游 -118 行（板级分支重整），wk/epaper/mr-IN/notify/control_mqtt 5 处条目要逐个确认 |
| `main/display/display.h` | 上游 +50 行新基类接口，`SetStatusColor` 是否仍在 |
| `main/boards/common/wifi_board.cc` | `kDeviceStateNotifying` 是否仍在复位条件里 |

---

## 7. 迁移执行方案

### 7.1 分支策略

```bash
# 一次性建立可复用的上游基线（放在仓库外，避免污染工作区）
cd /tmp && mkdir -p xz && cd xz
curl -sL -o v242.tar.gz https://codeload.github.com/78/xiaozhi-esp32/tar.gz/refs/tags/v2.4.2
curl -sL -o v252.tar.gz https://codeload.github.com/78/xiaozhi-esp32/tar.gz/refs/tags/v2.5.2
tar xzf v242.tar.gz && tar xzf v252.tar.gz
```

在本地仓库内（推荐，可保留逐块回退能力）：

```bash
cd /Users/tong/Downloads/xiaozhi-esp32-main
git checkout -b migrate/2.5.2
# 1) 先提交当前工作区（11 个已跟踪文件改动 + 13 个未跟踪项，先锁一个干净起点）
git add -A && git commit -m "chore: freeze fork 2.4.2.1 worktree before 2.5.2 migration"
# 2) 建立上游基线两支
git checkout -b upstream/base-2.4.2 <某次导入点>   # 或用 --allow-unrelated 造一个空基线
```

> 更省事的做法：不做 git 血统重建，而是**直接以 2.5.2 为新的工作树**，按第 6 节逐文件把 27 个定制文件搬过去。因为定制点已经精确定位（27 文件 / 19 个上游未动），搬运比反向合并更可控。

### 7.2 推荐顺序（每步都能独立编译）

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P0 备份** | 打 tag `fork-2.4.2.1`；`cp -r` 一份完整工作区 | `git tag -l` |
| **P1 骨架** | 取 2.5.2 全量 → `PROJECT_VER=2.5.2.1` → `idf_component.yml` 用上游版本 | 空板（如 `bread-compact-wifi`）能 `idf.py build` |
| **P2 板级** | 搬 3.49-V2 / 4B 两板全部定制（19 个上游未动文件里的大部分）+ `board.h`/`backlight`/`wifi_board` 新接口 | 两板 `build.py <board> --name <variant>` 通过 |
| **P3 显示** | `lcd_display`（视口/壁纸）、`lvgl_display`（状态色/时钟/电量）、`custom_lcd_display`×2、`oled_display` | 真机：预览拖动、壁纸、双击/三击、logo、时钟 |
| **P4 播报** | `notify/notify_player.cc` 改 `expected` API；`application.cc` 播报状态机 | 真机：`notify` 全流程 + 本地三手势停止 |
| **P5 控制通道** | `control_mqtt.{cc,h}` + 6 处挂钩（含 `application.cc` 冲突块 3/4/5） | `query_status` 字段齐全；15 条指令逐条过 |
| **P6 构建脚本** | `build.py` 3 项 + Kconfig（含 H264 占位符）+ CMakeLists 5 处 | `python3 -m unittest discover -s scripts/tests -v` 全绿 |
| **P7 回归** | 全量编译矩阵 + 真机清单（见 7.3） | 发布 `2.5.2.1` |

### 7.3 回归验证清单（按 AGENTS.md 的 Validation 要求）

**编译**：`3.49-V2`、`4B`、`wk-esp32s3-dev`（OLED/LCD 两变体）、`esp32-c6-epaper-1.54`、`bread-compact-wifi`（回归板），加 `python3 -m unittest discover -s scripts/tests -v`。

**必须真机验证**（编译成功不等于硬件通过）：

1. 背光守护：运行中手动复位 TCA9554（或断电瞬间恢复）→ 屏幕应自愈，日志出现 `Backlight PWM out of sync` / `Display enable lost`；
2. 显示使能守护：同上，`EXIO1/EXIO5 levels=0x22` 首条日志必须出现；
3. flush 死锁自恢复：人为构造超时（或长时间高负载刷屏）→ 20 s 后应自动重启而不是黑屏挂死；
4. 播报闭环：`notify` + `id` → 播放完成 / 双击停 / BOOT 停 / IMU 停 / `abort_notify`，5 种 `outcome` 全对；
5. 图片：预览 JPEG/PNG 拖动、背景 JPEG/PNG cover 裁切、`set_image clear`、双击清图不误清新图；
6. 主题：三击 light↔dark，logo 换深/浅版本，壁纸不丢；
7. IMU：姿态触发聆听；点击/按键抖动**不**误触发；
8. RTC：断网重启后时间保持（UTC+8 偏移正确）；
9. 电池：`query_status` 电量百分比与充电态；
10. BOOT 8 s 长按：清 NVS 后重启进入激活流程；
11. **自动 OTA 必须仍然关闭**：配置一台"服务端有更高版本"的环境，确认设备**不**自动升级。

---

## 8. 风险登记

| # | 风险 | 概率 | 影响 | 对策 |
|---|---|---|---|---|
| R1 | `notify_player.cc` 的 `expected` 适配引入新的错误处理遗漏（`Open` 失败分支） | 中 | 播报静默失败 | 逐一对照上游 2.5.2 原文件抄写，只保留 `id`/进度回调增量 |
| R2 | 自动 OTA 升级块被合并"修回来" | **中高** | 设备被小智云刷成官方固件，定制全丢 | 在 `application.cc` 加 `#if 0` 注释锚点 + 回归第 11 项 |
| R3 | `esp-ml307 3.7.5` 其他接口（`Network`/`Mqtt`）也变了，`control_mqtt.cc` 编译不过 | 中 | 控制通道不可用 | 先单独编 `control_mqtt.cc`；`Mqtt` 若改签名需同步 `Subscribe/Publish` 调用 |
| R4 | `lcd_display.cc` 的 `preview_viewport_` 与上游 +32 行相互覆盖，出现"图能显示但不能拖" | 中 | 功能降级 | 迁移后 `grep preview_viewport_` 应仍为 16 处 |
| R5 | `build.py` 的 wake word 判定与 `esp-sr ==2.4.7` 钉版冲突 | 低 | 变体列表报错 | 跑 `--list-boards --json` 与单测 |
| R6 | `uart-eth-modem` 从 `==0.6.0` 升到 `~0.7.0`，NT26/4G 板回归 | 低 | 该板不可用（非主线） | 若不用 4G 板可暂不验证，但需在文档标注 |
| R7 | `esp_video 2.4.1` 重写后本地唯一的 `Capture failed: camera did not initialize` 日志补丁丢失 | 低 | 摄像头排障少一条关键日志 | 迁移后 `grep "camera did not initialize" main/boards/common/esp_video.cc` 应为 1 处（RGB565X 支持上游已自带，无需再补） |

## 9. 待确认（需产品/硬件决策）

1. **4B 是否随本次一起发布？** 4B 的 7 个定制文件上游全未改动，搬运零风险，但 4B 尚未做过完整真机回归（发布记录里只有 3.49-V2 的台账）。
2. **wk-esp32s3-dev / esp32-c6-epaper-1.54 是否保留？** 两者都是 2.5.2 原样带入，与业务无关；保留会让 `--list-boards` 变长，删除可减小编译矩阵。
3. **`ota.cc` / `mqtt_protocol.cc` 的本地硬化是否还要另立分支？** 上游已等价实现，建议直接放弃本地版本，减少长期维护面。
4. **`docs/` 下 14 份 fork 文档是否随固件仓库继续维护？** 其中 `xiaozhi-presentation.pdf/html`、客户手册 PPTX 属于市场材料，与固件同仓会让每次 `rsync` 对比变慢（本次已排除 `build/` 等，仍需注意）。
