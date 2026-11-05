#include "i2c_device.h"

#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "I2cDevice"

I2cDevice::I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
    : i2c_bus_(i2c_bus), device_address_(addr) {
    i2c_device_config_t i2c_device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags =
            {
                .disable_ack_check = 0,
            },
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &i2c_device_cfg, &i2c_device_));
    assert(i2c_device_ != NULL);
}

// 这条 I2C 总线上挂着 PMIC、IO 扩展器、触摸、IMU、音频编解码和 RTC 共 7 个器件，
// 偶发超时是常态。上游用 ESP_ERROR_CHECK 处理，任何一次抖动都会 abort 整机重启
// （实测空载每分钟左右一次，语音会话时更频繁）。
// 这里改为：重试 3 次，仍失败则告警并按安全值返回，绝不让单次总线抖动杀死设备。
// 注意不要调用 ResetBus()：总线复位会打断同一条总线上其它器件的状态机。
static constexpr int kI2cRetryCount = 3;

void I2cDevice::WriteReg(uint8_t reg, uint8_t value) {
    uint8_t buffer[2] = {reg, value};
    for (int attempt = 0; attempt < kI2cRetryCount; ++attempt) {
        if (i2c_master_transmit(i2c_device_, buffer, 2, 100) == ESP_OK) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGW(TAG, "WriteReg(0x%02x) failed, dropped", reg);
}

void I2cDevice::WriteRegs(uint8_t reg, const uint8_t* buffer, size_t length) {
    uint8_t* tx = (uint8_t*)malloc(length + 1);
    if (tx == nullptr) {
        return;
    }
    tx[0] = reg;
    memcpy(tx + 1, buffer, length);
    for (int attempt = 0; attempt < kI2cRetryCount; ++attempt) {
        if (i2c_master_transmit(i2c_device_, tx, length + 1, 100) == ESP_OK) {
            free(tx);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGW(TAG, "WriteRegs(0x%02x) failed, dropped", reg);
    free(tx);
}

uint8_t I2cDevice::ReadReg(uint8_t reg) {
    uint8_t buffer[1] = {0};
    for (int attempt = 0; attempt < kI2cRetryCount; ++attempt) {
        if (i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, 1, 100) == ESP_OK) {
            return buffer[0];
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGW(TAG, "ReadReg(0x%02x) failed, returning 0", reg);
    return 0;
}

void I2cDevice::ReadRegs(uint8_t reg, uint8_t* buffer, size_t length) {
    for (int attempt = 0; attempt < kI2cRetryCount; ++attempt) {
        if (i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, length, 100) == ESP_OK) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGW(TAG, "ReadRegs(0x%02x) failed, zeroed", reg);
    memset(buffer, 0, length);
}

esp_err_t I2cDevice::ResetBus(const char* reason) {
    ESP_LOGW(TAG, "Resetting I2C bus: %s", reason ? reason : "unspecified");
    return i2c_master_bus_reset(i2c_bus_);
}
