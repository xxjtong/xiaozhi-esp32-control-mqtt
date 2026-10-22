#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include "lcd_display.h"

// // SPI LCD显示器
class CustomLcdDisplay : public LcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);

    // 重写 SetupUI：调用父类初始化后，注册双击切换主题事件
    virtual void SetupUI() override;

    // 重写 SetEmotion：开机/空闲时用公司 logo 全屏替代默认机器人图标
    virtual void SetEmotion(const char* emotion) override;
    // 主题切换后 logo 需要换成深/浅色版本
    virtual void SetTheme(Theme* theme) override;

private:
    static bool lvgl_port_flush_io_ready_callback(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
    static void lvgl_port_flush_callback(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map);
    // 屏幕点击事件回调（自行判定双击，见 .cc 中的说明）
    static void OnScreenClickEvent(lv_event_t* e);
    // 递归给已有对象树补上事件冒泡标志，使任意层级的双击都能冒泡到 container_
    static void EnableEventBubbling(lv_obj_t* obj);
    // 监听新建对象（如聊天消息气泡），动态补上事件冒泡标志
    static void OnChildCreatedEvent(lv_event_t* e);
    // 切换主题（light <-> dark）
    void ToggleTheme();
    // 全屏显示公司 logo（按当前主题选择深/浅色版本）
    void ShowCompanyLogo();
    // 当前是否正在显示 logo（主题切换时用于判断是否需要重绘）
    bool showing_logo_ = false;
    // 上一次短按的 tick，用于自行判定双击（0 表示还没有上一次点击）
    uint32_t last_click_tick_ = 0;
    // 两次点击间隔在该窗口内算双击
    static constexpr uint32_t DOUBLE_CLICK_WINDOW_MS = 500;
};

#endif // __CUSTOM_LCD_DISPLAY_H__
