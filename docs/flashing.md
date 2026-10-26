# 固件烧录与真机验证

针对本 fork（`waveshare/esp32-s3-touch-lcd-3.49-v2`）的烧录流程。所有命令均在
macOS + ESP-IDF v6.0.2 上实测通过。

> **厂家文档**：硬件规格、原理图、示例与排查手册见
> <https://docs.waveshare.net/ESP32-S3-Touch-LCD-3.49/>（完整链接清单见 §10）。
> 本文档只记录本项目相关的烧录要点，硬件细节以厂家文档为准。

---

## 1. 硬件识别

官方规格（[微雪文档](https://docs.waveshare.net/ESP32-S3-Touch-LCD-3.49/)）：

| 项 | 规格 |
|---|---|
| 主控 | ESP32-S3R8，Xtensa 32 位 LX7 双核，240 MHz |
| Flash | 16 MB（W25Q128JVSI） |
| PSRAM | 8 MB（芯片叠封） |
| 屏幕 | 3.49" IPS 电容触屏，172 × 640，AXS15231B（QSPI + I2C） |
| 音频 | ES8311 DAC + ES7210 ADC + 双数字麦克风阵列（支持 AEC） |
| IMU / RTC | QMI8658 六轴 / PCF85063 |
| 烧录与日志口 | Type-C（同一接口，既烧录也打印日志） |

esptool 实测：

```
Chip type:          ESP32-S3 (QFN56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 8MB (AP_3v3)
Detected flash size: 16MB
USB mode:           USB-Serial/JTAG
MAC:                80:45:6b:35:c8:9c
```

macOS 上为原生 USB-Serial/JTAG，**无需安装任何串口驱动**，端口形如 `/dev/cu.usbmodem1101`。

查找端口：

```sh
ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial*
```

若同时接了多块板子，用 `python3 -m esptool --chip esp32s3 -p <端口> flash-id`
读出的 MAC 来区分。

### 1.1 按键位置与下载模式

BOOT 与 RESET 按键均在**板子背面**（另有 PWR 键用于锂电池供电下的电源控制）。

官方给出的进入下载模式方法：

> 按住 BOOT 按键，单击 RESET 按键，即可进入下载模式。

正常情况下 esptool 会通过 USB-Serial/JTAG **自动**进入下载模式，无需手动操作；
只有在自动失败时才用上面的按键组合。

---

## 2. 环境准备

```sh
export PATH=/opt/homebrew/bin:$PATH
source /Users/tong/.espressif/v6.0.2/esp-idf/export.sh
```

**必须先让系统 Python 3.14 优先**：IDF 的虚拟环境是 `idf6.0_py3.14_env`，
若 `python3` 先解析到 3.13，`export.sh` 会去找不存在的 `idf6.0_py3.13_env` 而失败。

---

## 3. 编译

```sh
cd build && ninja all
```

> **不要用 `idf.py build`**：本环境下它会因 `os.mkdir` 对已存在的 `build/log`
> 报 `PermissionError(EEXIST)`。这是工具沙箱的问题，不是项目问题。
> `ninja` 直接构建可完全绕开，产物一致。

编译通过后关注输出末尾：

```
xiaozhi.bin binary size 0x2efbf0 bytes. Smallest app partition is 0x3f0000 bytes. 0x100410 bytes (25%) free.
```

---

## 4. 烧录偏移表

来源：`build/flash_args`

| 偏移 | 文件 | 说明 |
|---|---|---|
| `0x0` | `bootloader/bootloader.bin` | 二级 bootloader，极少变动 |
| `0x8000` | `partition_table/partition-table.bin` | 分区表 |
| `0xd000` | `ota_data_initial.bin` | OTA 数据区初始镜像 |
| `0x800000` | `generated_assets.bin` | 字体、音效等生成资源（8 MB 处） |
| **`0x20000`** | `xiaozhi.bin` | **应用程序**（注意不是常见的 `0x10000`） |

Flash 参数：`--flash-mode dio --flash-freq 80m --flash-size 16MB`

---

## 5. 常用命令

**检测连接（只读，安全）**

```sh
python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 flash-id
```

**仅刷 app（日常迭代推荐，约 16 秒）**

```sh
cd build
python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 -b 460800 \
    write-flash 0x20000 xiaozhi.bin
```

只改代码、未动资源时用它。**不擦除 NVS**，设备的配网信息与
`control_mqtt` 配置全部保留，重启后直接就能连 broker。

**全量烧录（约 3–5 分钟）**

```sh
cd build
python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 -b 460800 \
    write-flash \
    0x0      bootloader/bootloader.bin \
    0x8000   partition_table/partition-table.bin \
    0xd000   ota_data_initial.bin \
    0x800000 generated_assets.bin \
    0x20000  xiaozhi.bin
```

适用于：更换板型、改动分区表、修改字体/音效/资源，或不确定设备上基线版本时。

> esptool v5 起 `write_flash` 已更名 `write-flash`（旧名仍可用但会告警）。

---

## 6. 注意事项

### 6.1 不要轻易整片擦除

`erase_flash` 会清空 NVS，后果：

- 丢失 `control_mqtt` 命名空间里的 `endpoint` / `client_id` / `username` / `password` /
  `subscribe_topic` / `reply_topic`
- 丢失已保存的 WiFi 配网信息，设备需要重新配网
- 设备会回落到 Kconfig 默认值（`mqtt.virsical.cn:21883`、`admin` / `public`）

仅在排查疑难问题时使用，之后要重新注入 NVS 配置。

### 6.2 改了资源才需要重刷 assets

`generated_assets.bin` 由字体、音效、语言包等生成。**只在改动这些内容时才需要重刷**：

- `main/assets.cc`、字体配置（`CONFIG_..._FONT`）
- `main/assets/lang_config.h`（生成物）
- 音效文件

仅修改 C++ 逻辑时刷 app 即可，能省下几分钟和 8 MB 的写入。

### 6.3 app 偏移是 0x20000

写错成 `0x10000` 会导致设备无法启动（那里不是 app 分区起始）。
每次单独刷 app 前建议对照 `build/flash_args` 确认。

### 6.4 烧录失败的处理

- 自动进入下载模式失败时：**按住背面的 BOOT 键，单击背面的 RESET 键**（官方方法），然后重试
- 波特率过高不稳定时：把 `-b 460800` 降到 `-b 115200`
- 端口被占用：确认没有串口监视工具（monitor / miniterm / screen）正在连接
- 电池供电时若设备未开机，先按 PWR 键开机，或改用 Type-C 供电

### 6.5 硬件版本必须与固件匹配（V1 / V2）

**V1 已于 2026-06-08 停产，现在发货的都是 V2。** 两者引脚定义有差异：

- LCD_TE 与 LCD_RESET 互换
- LCD_BL（背光）与 EXIO_INT 互换
- V2 引出 TP_INT

版本识别：PCB 丝印 `Rev1.1`，或外壳 QC 标签 `V2`。

**刷错版本固件会直接导致屏幕点不亮**（背光引脚不同）。本仓库**同时存在 V1 与 V2 两个板型**，
务必确认选中的是 V2：

```sh
python3 scripts/build.py --list-boards | grep 3.49
#   waveshare/esp32-s3-touch-lcd-3.49        ← V1（已停产）
#   waveshare/esp32-s3-touch-lcd-3.49-v2     ← V2，本项目用这个

python3 scripts/build.py waveshare/esp32-s3-touch-lcd-3.49-v2
```

编译输出里也能确认：`-DBOARD_NAME="esp32-s3-touch-lcd-3.49-v2"`。

> 注意：`build.py` 会修改本地 `sdkconfig` 与构建状态，切换板型后不要想当然认为
> `build/` 仍代表上一次的目标。

---

## 7. 刷后验证

读取串口日志（115200 8N1）：

```python
python3 -c "
import serial, time
s = serial.Serial('/dev/cu.usbmodem1101', 115200, timeout=1)
end = time.time() + 25
while time.time() < end:
    d = s.read(4096)
    if d: print(d.decode('utf8','ignore'), end='')
"
```

正常启动应看到（本机实测）：

```
I (47459) ControlMqtt: Connected to control broker
I (47459) ControlMqtt: hello sent
```

看到这两行说明：固件已运行、控制通道已连上 broker、NVS 配置完好。

---

## 8. 获取设备的 MQTT 地址

控制通道的 topic 由设备 UUID 决定：

- 订阅（下行指令）：`xiaozhi/<UUID>/cmd`
- 发布（上行消息）：`xiaozhi/<UUID>/reply`
- MQTT client_id：`xiaozhi-<UUID后8位>`

最省事的取法：订阅 broker 上的 reply topic 或通配 `xiaozhi/+/reply`，
设备上线时会发 `hello`，其中的 `device_id` 就是完整 UUID。

本机没有 `mosquitto_pub` / `mosquitto_sub`，可用 paho-mqtt：

```sh
/Users/tong/.workbuddy/binaries/python/envs/default/bin/python -c "import paho.mqtt"
```

下发示例（订阅 reply 后发 `query_status`）：

```json
{ "type": "query_status" }
```

回执中应带有本次新增的 `notify_active` 字段，可用于确认新固件已生效。

---

## 9. 播报功能真机回归

详见 `docs/control-mqtt-notify-feedback.md` §7。最小验证顺序：

1. `query_status` → 确认 `notify_active` 字段存在（不需要音频）
2. 准备一条单声道 16 kHz Ogg Opus（≤5 秒），用本地 HTTP 托管（**URL 必须直链，不能有 302**）
3. 下发带 `id` 的 `notify` → 听声音，看 `reply` 回执与 `notify_result`
4. 播报中下发 `abort_notify` → 声音应立即停，`outcome=cancelled`
5. 播报中说唤醒词 → `outcome=aborted`

---

## 10. 参考资料

### 10.1 厂家文档（微雪 Waveshare）

| 用途 | 链接 |
|---|---|
| **产品主页**（规格、特性、接口、按键、V1/V2 差异） | <https://docs.waveshare.net/ESP32-S3-Touch-LCD-3.49/> |
| 资料与文档（原理图、规格书、示例代码下载） | <https://docs.waveshare.net/ESP32-S3-Touch-LCD-3.49/Resources-And-Documents> |
| ESP-IDF 开发环境搭建（官方推荐 VS Code 插件） | <https://docs.waveshare.net/ESP32-S3-Touch-LCD-3.49/ESP-IDF> |
| Arduino 开发环境搭建与示例 | <https://docs.waveshare.net/ESP32-S3-Touch-LCD-3.49/Arduino> |
| 产品 FAQ 与问题排查 | <https://docs.waveshare.net/ESP32-S3-Touch-LCD-3.49/FAQ> |
| 技术支持（联系厂家） | <https://docs.waveshare.net/ESP32-S3-Touch-LCD-3.49/Technical-Support> |

硬件相关问题（引脚定义、外设、原理图、屏幕不亮、电池充电）优先查厂家文档；
本项目相关问题（控制通道、播报、构建变体）查下面 §10.2。

### 10.2 本仓库文档

| 内容 | 路径 |
|---|---|
| 第二路 MQTT 控制通道与全部指令 | `docs/control-mqtt.md` |
| 播报闭环改造（notify 反馈） | `docs/control-mqtt-notify-feedback.md` |
| 远程语音播报服务端设计评估 | `docs/tts-broadcast-design.md` |
| 自定义板型开发指南 | `docs/custom-board.md` |
