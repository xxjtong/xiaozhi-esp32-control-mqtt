#ifndef RTC_MONITOR_H
#define RTC_MONITOR_H

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <sys/time.h>
#include <time.h>

#include "config.h"

// PCF85063 实时时钟（I2C 地址 0x51，与音频/IMU 共用总线 GPIO47/48）
#define PCF85063_ADDR       0x51
#define PCF85063_REG_CTRL1  0x00
#define PCF85063_REG_SEC    0x04

// 联网校时用的 NTP 服务器
#define SNTP_SERVER         "ntp.aliyun.com"

// 负责 PCF85063 的读写与系统时钟同步：
//   1. 开机时把 RTC 时间同步到系统时钟，即使没有 WiFi 也能显示正确时间；
//   2. 联网后通过 SNTP 校时（项目已跳过版本检查，不再有服务器下发时间），
//      校时成功即回写 RTC，保证 RTC 越用越准。
class RtcMonitor {
public:
    bool Initialize(i2c_master_bus_handle_t bus) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = PCF85063_ADDR,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(bus, &dev_cfg, &rtc_dev_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add RTC device");
            return false;
        }

        // CTRL1 = 0：振荡器正常运行（STOP=0），不启用中断
        WriteReg(PCF85063_REG_CTRL1, 0x00);

        // 秒寄存器 bit7 为 OS（振荡器停止）标志，置位表示时间不可信
        uint8_t sec = ReadReg(PCF85063_REG_SEC);
        if (sec & 0x80) {
            ESP_LOGW(TAG, "RTC time invalid (OS flag set), waiting for network time");
        } else {
            tm rtc_tm;
            if (ReadTime(rtc_tm) && rtc_tm.tm_year >= 2025 - 1900) {
                struct timeval tv = {};
                tv.tv_sec = TmToEpoch(rtc_tm);
                settimeofday(&tv, nullptr);
                ESP_LOGI(TAG, "System time restored from RTC: %04d-%02d-%02d %02d:%02d:%02d",
                         rtc_tm.tm_year + 1900, rtc_tm.tm_mon + 1, rtc_tm.tm_mday,
                         rtc_tm.tm_hour, rtc_tm.tm_min, rtc_tm.tm_sec);
            }
        }

        xTaskCreate(RtcTaskTrampoline, "rtc_monitor", 4096, this, 2, nullptr);
        return true;
    }

private:
    static constexpr const char* TAG = "RtcMonitor";
    i2c_master_dev_handle_t rtc_dev_ = nullptr;

    static constexpr uint32_t SYNC_INTERVAL_MS = 30 * 1000;  // 检查周期
    static constexpr int64_t JUMP_THRESHOLD_SEC = 2;         // 判定被外部校时的跳变阈值

    uint8_t ReadReg(uint8_t reg) {
        uint8_t val = 0;
        i2c_master_transmit_receive(rtc_dev_, &reg, 1, &val, 1, -1);
        return val;
    }

    void WriteReg(uint8_t reg, uint8_t val) {
        uint8_t buf[2] = {reg, val};
        i2c_master_transmit(rtc_dev_, buf, 2, -1);
    }

    bool ReadTime(tm& out) {
        uint8_t reg = PCF85063_REG_SEC;
        uint8_t buf[7] = {0};
        if (i2c_master_transmit_receive(rtc_dev_, &reg, 1, buf, 7, -1) != ESP_OK) {
            return false;
        }
        out = {};
        out.tm_sec  = Bcd2Dec(buf[0] & 0x7F);
        out.tm_min  = Bcd2Dec(buf[1] & 0x7F);
        out.tm_hour = Bcd2Dec(buf[2] & 0x3F);  // 24 小时模式
        out.tm_mday = Bcd2Dec(buf[3] & 0x3F);
        out.tm_wday = Bcd2Dec(buf[4] & 0x07);
        out.tm_mon  = Bcd2Dec(buf[5] & 0x1F) - 1;
        out.tm_year = Bcd2Dec(buf[6]) + 100;   // 2000 年起算
        return true;
    }

    bool WriteTime(const tm& in) {
        uint8_t buf[8] = {
            PCF85063_REG_SEC,
            (uint8_t)(Dec2Bcd(in.tm_sec) & 0x7F),
            (uint8_t)(Dec2Bcd(in.tm_min) & 0x7F),
            (uint8_t)(Dec2Bcd(in.tm_hour) & 0x3F),
            (uint8_t)(Dec2Bcd(in.tm_mday) & 0x3F),
            (uint8_t)(Dec2Bcd(in.tm_wday) & 0x07),
            (uint8_t)(Dec2Bcd(in.tm_mon + 1) & 0x1F),
            (uint8_t)(Dec2Bcd(in.tm_year % 100)),
        };
        return i2c_master_transmit(rtc_dev_, buf, sizeof(buf), -1) == ESP_OK;
    }

    static uint8_t Dec2Bcd(int v) {
        return (uint8_t)(((v / 10) << 4) | (v % 10));
    }

    static int Bcd2Dec(uint8_t v) {
        return ((v >> 4) * 10) + (v & 0x0F);
    }

    // 把日历时间按“UTC 墙钟”换算为 epoch 秒。
    // 项目约定 settimeofday 存入的 epoch 即为本地墙钟时间
    // （见 ota.cc：server_time 直接加上 timezone_offset 后写入），
    // 故此处不能用 mktime（会受 TZ 影响），用纯算法换算保证一致。
    static time_t TmToEpoch(const tm& t) {
        static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int year = t.tm_year + 1900;
        int month = t.tm_mon + 1;
        long long days = 0;
        for (int y = 1970; y < year; ++y) {
            days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
        }
        bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        for (int m = 1; m < month; ++m) {
            days += mdays[m - 1] + ((m == 2 && leap) ? 1 : 0);
        }
        days += t.tm_mday - 1;
        return (time_t)(((days * 24 + t.tm_hour) * 60 + t.tm_min) * 60 + t.tm_sec);
    }

    static void RtcTaskTrampoline(void* arg) {
        static_cast<RtcMonitor*>(arg)->RtcTask();
    }

    // SNTP 同步回调：lwip 已先把 UTC 时间写入系统时钟，这里再叠加时区偏移，
    // 使系统 epoch 与项目约定（本地墙钟，见 ota.cc）保持一致。
    static void OnSntpSync(struct timeval* tv) {
        if (tv == nullptr) return;

        struct timeval local = *tv;
        local.tv_sec += RTC_TIMEZONE_OFFSET_SEC;
        settimeofday(&local, nullptr);

        time_t t = local.tv_sec;
        struct tm tm_local;
        gmtime_r(&t, &tm_local);
        ESP_LOGI(TAG, "SNTP synced: %04d-%02d-%02d %02d:%02d:%02d",
                 tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
                 tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
    }

    static bool NetworkReady() {
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        return netif != nullptr && esp_netif_is_netif_up(netif);
    }

    static void StartSntp() {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
        cfg.smooth_sync = false;   // 立即校时（跳变），便于回写 RTC
        cfg.wait_for_sync = false;
        cfg.sync_cb = OnSntpSync;
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SNTP started, server: %s", SNTP_SERVER);
        } else {
            ESP_LOGW(TAG, "Failed to start SNTP: %s", esp_err_to_name(err));
        }
    }

    void RtcTask() {
        // 以 ESP 单调时基为基准推算系统时钟的“应有值”。
        // 系统时钟本身由该时基推进，因此若两者出现跳变，
        // 说明有外部调用 settimeofday（SNTP 校时）→ 把新时间回写 RTC。
        int64_t base_us = esp_timer_get_time();
        time_t base_epoch = time(nullptr);
        bool sntp_started = false;

        while (true) {
            vTaskDelay(pdMS_TO_TICKS(SYNC_INTERVAL_MS));

            // 联网后启动一次 SNTP 校时（此后由 lwip 按小时自动续订）
            if (!sntp_started && NetworkReady()) {
                StartSntp();
                sntp_started = true;
            }

            time_t now = time(nullptr);
            time_t expected = base_epoch + (time_t)((esp_timer_get_time() - base_us) / 1000000);
            int64_t jump = (int64_t)now - (int64_t)expected;
            if (jump < 0) jump = -jump;
            if (jump <= JUMP_THRESHOLD_SEC) continue;  // 未被校时，保持 RTC 不受影响

            struct tm sys_tm;
            gmtime_r(&now, &sys_tm);
            if (sys_tm.tm_year >= 2025 - 1900 && WriteTime(sys_tm)) {
                ESP_LOGI(TAG, "RTC updated from system time: %04d-%02d-%02d %02d:%02d:%02d",
                         sys_tm.tm_year + 1900, sys_tm.tm_mon + 1, sys_tm.tm_mday,
                         sys_tm.tm_hour, sys_tm.tm_min, sys_tm.tm_sec);
            }

            // 重新设定基准，避免后续重复回写
            base_us = esp_timer_get_time();
            base_epoch = now;
        }
    }
};

#endif  // RTC_MONITOR_H
