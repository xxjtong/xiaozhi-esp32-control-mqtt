#ifndef IMU_MONITOR_H
#define IMU_MONITOR_H

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cmath>

#include "application.h"
#include "device_state.h"

// QMI8658 寄存器（仅启用加速度计，姿态变化检测）
#define QMI8658_ADDR            0x6B
#define QMI8658_REG_WHOAMI      0x00
#define QMI8658_REG_CTRL1       0x02
#define QMI8658_REG_CTRL2       0x03
#define QMI8658_REG_CTRL7       0x08
#define QMI8658_REG_AX_L        0x35
#define QMI8658_REG_RESET       0x60

class ImuMonitor {
public:
    bool Initialize(i2c_master_bus_handle_t bus, bool enabled = true) {
        enabled_ = enabled;
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = QMI8658_ADDR,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(bus, &dev_cfg, &imu_dev_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add IMU device");
            return false;
        }

        // 检查 WHO_AM_I
        uint8_t whoami = ReadReg(QMI8658_REG_WHOAMI);
        if (whoami != 0x05) {
            ESP_LOGE(TAG, "QMI8658 not found (whoami=0x%02x)", whoami);
            return false;
        }

        // Reset
        WriteReg(QMI8658_REG_RESET, 0xB0);
        vTaskDelay(pdMS_TO_TICKS(20));

        // CTRL1: 启用地址自动增量 (bit6)
        WriteReg(QMI8658_REG_CTRL1, 0x40);

        // CTRL2: 加速度计 4G 量程 + 1000Hz ODR
        //   bit7-4: range (0x10 = 4G), bit3-0: ODR (0x03 = 1000Hz)
        WriteReg(QMI8658_REG_CTRL2, 0x13);

        // CTRL7: 启用加速度计 (bit0)
        WriteReg(QMI8658_REG_CTRL7, 0x01);

        ESP_LOGI(TAG, "QMI8658 initialized");

        // 启动监控任务
        if (xTaskCreate(ImuTaskTrampoline, "imu_monitor", 4096, this, 2, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create IMU task");
            return false;
        }
        return true;
    }

    // Runtime switch used by the control channel (set_imu); persisted by the board.
    void SetEnabled(bool enabled) { enabled_ = enabled; }
    bool IsEnabled() const { return enabled_.load(); }

private:
    static constexpr const char* TAG = "ImuMonitor";
    i2c_master_dev_handle_t imu_dev_ = nullptr;

    // 状态机：检测运动 → 触发 StartListening
    float last_mag_ = 1.0f;          // 上次加速度幅值（g），静态约 1g
    int64_t last_trigger_us_ = 0;    // 上次触发时间
    std::atomic<bool> enabled_{true};
    static constexpr int64_t COOLDOWN_US = 5 * 1000 * 1000;  // 5 秒冷却
    static constexpr float MOTION_THRESHOLD = 0.1f;  // 幅值变化阈值（g），极轻微动作即触发

    uint8_t ReadReg(uint8_t reg) {
        uint8_t val = 0;
        i2c_master_transmit_receive(imu_dev_, &reg, 1, &val, 1, -1);
        return val;
    }

    void WriteReg(uint8_t reg, uint8_t val) {
        uint8_t buf[2] = {reg, val};
        i2c_master_transmit(imu_dev_, buf, 2, -1);
    }

    bool ReadAccel(float& x, float& y, float& z) {
        uint8_t reg = QMI8658_REG_AX_L;
        uint8_t buf[6] = {0};
        if (i2c_master_transmit_receive(imu_dev_, &reg, 1, buf, 6, -1) != ESP_OK) {
            return false;
        }
        int16_t rx = (int16_t)((buf[1] << 8) | buf[0]);
        int16_t ry = (int16_t)((buf[3] << 8) | buf[2]);
        int16_t rz = (int16_t)((buf[5] << 8) | buf[4]);
        // 4G 量程: 32768 = 4g → 1g = 8192
        x = rx / 8192.0f;
        y = ry / 8192.0f;
        z = rz / 8192.0f;
        return true;
    }

    static void ImuTaskTrampoline(void* arg) {
        static_cast<ImuMonitor*>(arg)->ImuTask();
    }

    void ImuTask() {
        vTaskDelay(pdMS_TO_TICKS(500));  // 等待系统启动
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(200));  // 5Hz 采样

            float x, y, z;
            if (!ReadAccel(x, y, z)) continue;

            float mag = sqrtf(x * x + y * y + z * z);
            float delta = fabsf(mag - last_mag_);
            last_mag_ = mag;

            // Keep the baseline fresh while disabled, but never trigger. This makes
            // runtime enable/disable through set_imu safe without a spurious event.
            if (!enabled_.load()) continue;

            // 冷却期内不触发
            int64_t now = esp_timer_get_time();
            if (now - last_trigger_us_ < COOLDOWN_US) continue;

            // 只在 Idle 状态触发（避免打断 listening/speaking）
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateIdle) continue;

            // 加速度幅值变化超过阈值 → 姿态变化
            if (delta > MOTION_THRESHOLD) {
                ESP_LOGI(TAG, "Motion detected: mag=%.2fg, delta=%.2fg → StartListening",
                         mag, delta);
                last_trigger_us_ = now;
                app.ReportUserEvent("motion_detected");
                app.Schedule([]() {
                    Application::GetInstance().ToggleChatState();
                });
            }
        }
    }
};

#endif  // IMU_MONITOR_H
