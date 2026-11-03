#pragma once

#include <cstdint>
#include <functional>

#include <driver/gpio.h>
#include <esp_timer.h>


class Backlight {
public:
    Backlight();
    ~Backlight();

    void RestoreBrightness();
    void SetBrightness(uint8_t brightness, bool permanent = false);
    inline uint8_t brightness() const { return brightness_; }

    // 把当前亮度重新写进硬件。SetBrightness() 在 brightness_ 未变化时会直接返回，
    // 因此一旦 PWM/外设被复位导致实际占空比丢失，软件侧状态仍旧「正确」，永远修不回来
    // （表现为背光全灭、屏黑，但设备其它功能正常，只能重启）。守护任务周期性调用本函数兜底。
    // 返回 true 表示硬件值确实与记录不一致、已修复；返回 false 表示本来就是对的。
    virtual bool ReapplyBrightness();

protected:
    void OnTransitionTimer();
    virtual void SetBrightnessImpl(uint8_t brightness) = 0;

    esp_timer_handle_t transition_timer_ = nullptr;
    uint8_t brightness_ = 0;
    uint8_t target_brightness_ = 0;
    uint8_t step_ = 1;
};


class PwmBacklight : public Backlight {
public:
    PwmBacklight(gpio_num_t pin, bool output_invert = false, uint32_t freq_hz = 25000);
    ~PwmBacklight();

    void SetBrightnessImpl(uint8_t brightness) override;
    bool ReapplyBrightness() override;
};
