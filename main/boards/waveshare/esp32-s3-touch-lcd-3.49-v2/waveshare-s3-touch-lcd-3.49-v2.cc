#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "settings.h"

#include <esp_log.h>
#include "i2c_device.h"
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <esp_timer.h>
#include "esp_io_expander_tca9554.h"

#include "esp_lcd_axs15231b.h"

#include "custom_lcd_display.h"
#include "adc_battery_monitor.h"
#include "imu_monitor.h"
#include "rtc_monitor.h"

#include <lvgl.h>

#define TAG "waveshare_lcd_3_49_v2"


static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t []){0x00}, 0, 100},
    {0x29, (uint8_t []){0x00}, 0, 100},
};

class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    Button pwr_button_;
    i2c_master_bus_handle_t i2c_bus_;
    esp_io_expander_handle_t io_expander = NULL;
    LcdDisplay* display_;
    i2c_master_dev_handle_t disp_touch_dev_handle = NULL;
    lv_indev_t *touch_indev = NULL;    //touch
    bool is_PwrControlEn = false;
    AdcBatteryMonitor* adc_battery_monitor_ = nullptr;
    ImuMonitor imu_monitor_;
    bool imu_available_ = false;
    RtcMonitor rtc_monitor_;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);
        if(ret != ESP_OK)
        ESP_LOGE(TAG, "TCA9554 create returned error");
        ret = esp_io_expander_set_dir(io_expander,
                                      IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_5 |
                                          IO_EXPANDER_PIN_NUM_6 | IO_EXPANDER_PIN_NUM_7,
                                      IO_EXPANDER_OUTPUT);
        ESP_ERROR_CHECK(ret);
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_1, 0);
        ESP_ERROR_CHECK(ret);
        ret = esp_io_expander_set_level(io_expander,
                                        IO_EXPANDER_PIN_NUM_5 | IO_EXPANDER_PIN_NUM_6 |
                                            IO_EXPANDER_PIN_NUM_7,
                                        1);
        ESP_ERROR_CHECK(ret);
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize QSPI bus");
        spi_bus_config_t buscfg = {};
        buscfg.data0_io_num = LCD_D0;
        buscfg.data1_io_num = LCD_D1;
        buscfg.data2_io_num = LCD_D2;
        buscfg.data3_io_num = LCD_D3;
        buscfg.sclk_io_num = LCD_PCLK;
        buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGI(TAG, "Install LCD driver");
        const axs15231b_vendor_config_t vendor_config = {
            .init_cmds = lcd_init_cmds, // Uncomment these line if use custom initialization commands
            .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.vendor_config = (void *)&vendor_config;
        esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &panel);

        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_5, 1));
        vTaskDelay(pdMS_TO_TICKS(30));
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_5, 0));
        vTaskDelay(pdMS_TO_TICKS(250));
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_5, 1));
        vTaskDelay(pdMS_TO_TICKS(30));
        esp_lcd_panel_init(panel);

        display_ = new CustomLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_1, 1));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            // Pressing the button shakes the board too; keep the IMU from reading it
            // as a posture change (see ImuMonitor::NotifyUserInteraction).
            ImuMonitor::NotifyUserInteraction();
            auto& app = Application::GetInstance();
            app.ReportUserEvent("button_click", "boot");
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // 长按 BOOT 键 8 秒：清除服务器下发的协议配置并重启。
        // 用于配置失效（如服务器更换地址、配置损坏）导致设备无法连接时恢复：
        // 重启后 MQTT/WebSocket 配置为空，会重新执行版本检查与激活流程。
        boot_button_.OnLongPress([]() {
            ESP_LOGW(TAG, "BOOT long-press detected, resetting server config");
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                app.Alert("重置配置", "已清除服务器配置，正在重启…", "neutral");

                Settings mqtt_settings("mqtt", true);
                mqtt_settings.EraseAll();
                Settings websocket_settings("websocket", true);
                websocket_settings.EraseAll();

                vTaskDelay(pdMS_TO_TICKS(800));
                app.Reboot();
            });
        });

        pwr_button_.OnLongPress([this]() {
            if(is_PwrControlEn) {
                is_PwrControlEn = false;
                esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_6, 0);
            }
        });

        pwr_button_.OnPressUp([this]() {
            ImuMonitor::NotifyUserInteraction();
            if(!is_PwrControlEn) {
                is_PwrControlEn = true;
            }
        });
    }

    void InitializeTouch() {
        i2c_master_bus_handle_t touch_i2c_bus_;
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {};
            i2c_bus_cfg.i2c_port = (i2c_port_t)I2C_NUM_1;
            i2c_bus_cfg.sda_io_num = I2C_Touch_SDA_PIN;
            i2c_bus_cfg.scl_io_num = I2C_Touch_SCL_PIN;
            i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
            i2c_bus_cfg.glitch_ignore_cnt = 7;
            i2c_bus_cfg.intr_priority = 0;
            i2c_bus_cfg.trans_queue_depth = 0;
            i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &touch_i2c_bus_));

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = I2C_Touch_ADDRESS,
            .scl_speed_hz = 300000,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(touch_i2c_bus_, &dev_cfg, &disp_touch_dev_handle));

        touch_indev = lv_indev_create();
        lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(touch_indev, TouchInputReadCallback);
        lv_indev_set_user_data(touch_indev, disp_touch_dev_handle);
    }

    static void TouchInputReadCallback(lv_indev_t * indev, lv_indev_data_t *indevData) {
        i2c_master_dev_handle_t i2c_dev = (i2c_master_dev_handle_t)lv_indev_get_user_data(indev);
        uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e,0x0, 0x0, 0x0};
        uint8_t buff[32] = {0};
        i2c_master_transmit_receive(i2c_dev,read_touchpad_cmd,11,buff,32,1000);
        uint16_t pointX;
        uint16_t pointY;
        pointX = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
        pointY = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
        if (buff[1]>0 && buff[1]<5) {
            indevData->state = LV_INDEV_STATE_PRESSED;
            if(pointX > DISPLAY_WIDTH) pointX = DISPLAY_WIDTH;
            if(pointY > DISPLAY_HEIGHT) pointY = DISPLAY_HEIGHT;
            indevData->point.x = pointY;
            indevData->point.y = (DISPLAY_HEIGHT-pointX);
            ESP_LOGD(TAG, "Touch point: (%ld,%ld)", (long)indevData->point.x, (long)indevData->point.y);
        } else {
            indevData->state = LV_INDEV_STATE_RELEASED;
        }
    }

    void GetPwrCurrentState() {
        if(gpio_get_level(PWR_BUTTON_GPIO)) {
            is_PwrControlEn = true;
        }
    }

    void InitializeBatteryMonitor() {
        adc_battery_monitor_ = new AdcBatteryMonitor(BATTERY_ADC_UNIT, BATTERY_ADC_CHANNEL,
                                                      BATTERY_ADC_UPPER_RESISTOR, BATTERY_ADC_LOWER_RESISTOR);
    }

    void InitializeImu() {
        // IMU 接在音频 I2C 总线上（GPIO 47/48），地址 0x6B
        // 可通过控制通道的 set_imu 命令或 NVS (imu/enabled) 关闭，避免调试时误触发。
        Settings settings("imu", false);
        bool enabled = settings.GetBool("enabled", true);
        imu_available_ = imu_monitor_.Initialize(i2c_bus_, enabled);
        if (!imu_available_) {
            ESP_LOGW(TAG, "IMU not initialized, gesture trigger disabled");
        } else if (!enabled) {
            ESP_LOGI(TAG, "IMU motion detection disabled by NVS");
        }
    }

    void InitializeRtc() {
        // RTC 同样接在音频 I2C 总线上（GPIO 47/48），地址 0x51
        if (!rtc_monitor_.Initialize(i2c_bus_)) {
            ESP_LOGW(TAG, "RTC not initialized, time will not persist offline");
        }
    }

public:
    CustomBoard() :
        // BOOT 键配置 8 秒长按（用于重置服务器配置，见 InitializeButtons）
        boot_button_(BOOT_BUTTON_GPIO, false, 8000),
        pwr_button_(PWR_BUTTON_GPIO) {
        InitializeI2c();
        InitializeTca9554();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeTouch();
        GetPwrCurrentState();
        InitializeBatteryMonitor();
        InitializeImu();
        InitializeRtc();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (adc_battery_monitor_ == nullptr) {
            return false;
        }
        // 本板无充电状态引脚，充电状态由 adc_battery_estimation 的软件趋势估算得出，
        // 而该估算只在 GetBatteryLevel() 内部推进。必须先取电量，再读充电状态，
        // 否则 IsCharging() 拿到的是上一轮的旧值。
        level = adc_battery_monitor_->GetBatteryLevel();
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        return true;
    }

    bool HasImu() override { return imu_available_; }

    bool SetImuEnabled(bool enabled) override {
        if (!imu_available_) {
            return false;
        }
        imu_monitor_.SetEnabled(enabled);
        Settings settings("imu", true);
        settings.SetBool("enabled", enabled);
        return true;
    }

    bool IsImuEnabled() override { return imu_available_ && imu_monitor_.IsEnabled(); }
};

DECLARE_BOARD(CustomBoard);
