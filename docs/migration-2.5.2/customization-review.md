# 定制功能评审与"最小化改动"建议

对比基准：`78/xiaozhi-esp32` 上游 2.5.2（本地 `/tmp/xz_upstream/xiaozhi-esp32-2.5.2`）
评审范围：`main/`、`scripts/`、根构建文件（不含 docs、assets 生成物）

**总量：39 个文件改动 + 6 个新增源文件。**

---

## 一、定制功能清单

### F1 控制通道（最大单项）

| 项 | 内容 |
|---|---|
| 新增 | `control_mqtt.cc` 1299 行、`control_mqtt.h` 142 行 |
| 指令 | listen / abort / mcp / system / set_image / set_theme / set_volume / set_brightness / toggle_wake_word / notify / abort_notify / set_status / toggle_power_save / set_imu / query_status |
| 被迫改动的上游文件 | `application.cc/h`（启动第二路 MQTT）、`main/CMakeLists.txt`、`board.h`、`wifi_board.*`、`dual_network_board.*`、`ethernet_board.h`、`display.h/cc`、`lvgl_display.*`、`lcd_display.*`、`device_state.h` |

### F2 显示能力扩展

| 能力 | 上游文件 | 规模 |
|---|---|---|
| 预览图（可上下拖动、超时清除） | `lcd_display.cc/h` | +249 / +8 |
| 壁纸（cover 缩放居中裁剪） | 同上 | 含在上面 |
| 状态文字颜色 | `display.h/cc`、`lvgl_display.cc` | +9 / +35 |

### F3 板级能力暴露

`board.h` +=11：`GetIpAddress` / `GetWifiSsid` / `GetWifiRssi` / `HasImu` / `SetImuEnabled` / `IsImuEnabled`
→ 由 `wifi_board`、`dual_network_board`、`ethernet_board`、4B 板分别实现。

### F4 激活体验

`application.cc` +109：配对码上屏 + 串口日志；`MarkCurrentVersionValid()` 补齐快速路径。

### F5 背光守护

`backlight.h/cc` +21：新增 `ReapplyBrightness()`，基类无条件重写、PWM 实现回读 LEDC 占空比比对后重写。

### F6 板级产品化

| 板 | 改动 | 内容 |
|---|---|---|
| 3.49-V2 | +158（板）+ 341/37（定制显示） | logo、长按重新激活、省电、IMU、RTC |
| 4B | +109（板）+ 323（定制显示） | 同上 + bounce buffer、无电池判定、永不关机 |

新增文件：`custom_lcd_display.*`、`company_logo.c/h`、`imu_monitor.h`、`rtc_monitor.h`、`assets/`

### F7 OLED 竖屏适配

`oled_display.h/cc` +102：新增 `swap_xy` 参数与 `SetupUI_128x32_Portrait()`；
连带修改 `bread-compact-wifi/config.h`（`#define DISPLAY_SWAP_XY true`）与其 `.cc`。

### F8 工程配置

`PROJECT_VER` → `2.5.2.1`、`Kconfig.projbuild` +26（新板型）、CI 测试适配 +27、`.gitignore` 调整。

### F9 本次技术修复（已实测验证）

| 文件 | 改动 | 是否必须 |
|---|---|---|
| `i2c_device.cc` | 重试 3 次 + 安全返回值 | **必须**（上游 40 秒必崩） |
| `afe_audio_engine.cc` | `VC + LOW_COST` | **必须**（上游 120 秒刷屏 92 条 + 3 次看门狗） |
| `lcd_display.cc` | 缓冲 50→30 行 | **必须**（否则语音 ENOMEM） |
| 4B 板 | bounce buffer 40 / 无电池 / 永不关机 | **必须**（闪屏、误报 0%、误断电） |

---

## 二、建议减少的定制（可与上游对齐）

| # | 对象 | 现状 | 建议 | 收益 |
|---|---|---|---|---|
| R1 | `bread-compact-wifi/config.h` + `.cc` | 加了 `DISPLAY_SWAP_XY true` | **回退**（该板与产品线无关，属调试残留） | 2 个文件归零 |
| R2 | `device_state.h` | 新增 `DeviceStateToString()` | 移到 `control_mqtt.cc` 内部静态函数 | 1 个文件归零 |
| R3 | `lcd_display.cc/h` 的预览图/壁纸 | 改在公共类（+257） | 下沉到 `CustomLcdDisplay`（fork 自有文件） | 2 个文件归零 |
| R4 | `lvgl_display.cc/h` 的 `SetStatusColor` | 改在公共类（+35） | 同上，下沉到 `LcdDisplay` 子类 | 2 个文件归零 |
| R5 | `backlight.h/cc` 的 `ReapplyBrightness` | 改在公共类（+21） | 改为 4B 板内定时器直接改亮度 | 2 个文件归零 |
| R6 | `sdkconfig.defaults` | 删了 LVGL calendar 两项 | 除非为省 flash，恢复 | 1 个文件归零 |
| R7 | `oled_display.cc` 的 `swap_xy` | +102 | 若无该类产品，回退 | 2 个文件归零 |

**合计可回收 12 个文件**（39 → 27），且都只是"搬家"，不改变功能。

---

## 三、不建议改的部分（虚函数方案已是最优）

`board.h` / `display.h` 的新增虚函数看似"侵入上游"，但因为工程用 `-fno-rtti`：

- 不能用 `dynamic_cast` 做能力探测
- 只能靠虚函数或 `static_cast` + 类型标签

因此**维持虚函数接口是当前约束下的最小侵入方案**，不建议改成注册表或接口分离（反而增加复杂度）。

---

## 四、优先级建议

| 优先级 | 项 | 理由 |
|---|---|---|
| P0 | R1 | 纯残留，风险最低、收益直接 |
| P1 | R2、R5、R6 | 改动小、无行为变化 |
| P2 | R3、R4 | 涉及显示重构，需回归预览图/壁纸/状态色 |
| P3 | R7 | 取决于是否还有 128×32 竖屏产品 |
| 不动 | F1、F3、F9 | 核心业务与实测必需的修复 |

---

## 五、结论

- 真正的"业务必需定制"是 F1（控制通道）与 F9（技术修复），其余多为产品化增强。
- 当前 39 个改动文件中，**约 12 个可以通过"把代码搬进 fork 自有文件"的方式归还上游**，不牺牲任何功能。
- 建议按 P0 → P2 分批执行，每批单独编译 + 单测 + 真机验证。
