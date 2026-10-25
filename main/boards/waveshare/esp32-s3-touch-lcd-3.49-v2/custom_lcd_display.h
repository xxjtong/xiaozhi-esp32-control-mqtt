#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include "lcd_display.h"

// // SPI LCD显示器
class CustomLcdDisplay : public LcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
    ~CustomLcdDisplay() override;

    // 重写 SetupUI：调用父类初始化后，注册双击清图 / 三击切主题事件
    virtual void SetupUI() override;

    // 重写 SetEmotion：开机/空闲时用公司 logo 全屏替代默认机器人图标
    virtual void SetEmotion(const char* emotion) override;
    // 主题切换后 logo 需要换成深/浅色版本
    virtual void SetTheme(Theme* theme) override;

private:
    static bool lvgl_port_flush_io_ready_callback(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
    static void lvgl_port_flush_callback(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map);
    // 屏幕点击事件回调（自行判定双击/三击，见 .cc 中的说明）
    static void OnScreenClickEvent(lv_event_t* e);
    // 双击等待窗口到期：确认不是三击后再清图
    static void OnClickTimer(lv_timer_t* timer);
    // 递归给已有对象树补上事件冒泡标志，使任意层级的点击都能冒泡到 container_
    static void EnableEventBubbling(lv_obj_t* obj);
    // 监听新建对象（如聊天消息气泡），动态补上事件冒泡标志
    static void OnChildCreatedEvent(lv_event_t* e);
    // 双击：清除远程下发的常驻预览图（无图时无动作）
    void ClearPreviewImage();
    // 三击：切换主题（light <-> dark）
    void ToggleTheme();
    // 全屏显示公司 logo（按当前主题选择深/浅色版本）
    void ShowCompanyLogo();
    // 当前是否正在显示 logo（主题切换时用于判断是否需要重绘）
    bool showing_logo_ = false;
    // 上一次短按的 tick（0 表示还没有上一次点击）
    uint32_t last_click_tick_ = 0;
    // 当前连续点击次数：1=单击，2=双击（清图），3=三击（切主题）
    uint8_t click_streak_ = 0;
    // 双击确认定时器：三击到达时取消，避免三击顺带清图
    lv_timer_t* click_timer_ = nullptr;
    // 待清除的预览图快照：若窗口内又下发了新图，不能把新图误清掉
    bool pending_clear_ = false;
    const LvglImage* pending_clear_target_ = nullptr;
    // 相邻两次点击间隔在该窗口内算同一次连续点击
    static constexpr uint32_t CLICK_WINDOW_MS = 500;
};

#endif // __CUSTOM_LCD_DISPLAY_H__
