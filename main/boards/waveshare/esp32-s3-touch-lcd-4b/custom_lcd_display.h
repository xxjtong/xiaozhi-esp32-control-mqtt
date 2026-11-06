#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include "lcd_display.h"

#include <atomic>

// 4B 板的"定制 LcdDisplay"：显示管线完全复用 RgbLcdDisplay（RGB 并口 + lvgl_port 的
// bounce buffer/双缓冲机制已经处理好刷新，不需要 3.49-V2 那套手写 flush），
// 这里只叠加板级交互能力：
//   1. SetupUI：注册屏幕双击/三击手势
//      - 双击：播报中停播报；有预览图清预览图；空闲/说话中进入聆听，聆听中返回空闲
//      - 三击：切换 light/dark 主题
//   2. SetEmotion/SetTheme：空闲与开机时用公司 logo 替代默认机器人图标
class CustomLcdDisplay : public RgbLcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
    ~CustomLcdDisplay() override;

    // 重写 SetupUI：调用父类初始化后，注册双击/三击手势
    virtual void SetupUI() override;

    // 重写 SetEmotion：开机/空闲时用公司 logo 全屏替代默认机器人图标
    virtual void SetEmotion(const char* emotion) override;
    // 主题切换后 logo 需要换成深/浅色版本
    virtual void SetTheme(Theme* theme) override;
    // 远程壁纸与公司 logo 互斥：有壁纸时隐藏 logo，清壁纸后恢复
    bool SetBackgroundImage(std::unique_ptr<LvglImage> image) override;
    void ClearBackgroundImage() override;

private:
    // 双击动作要等确认窗口结束才执行：窗口内来了第三击就改成三击切主题。
    enum class DoubleClickAction : uint8_t {
        kNone = 0,
        kStopNotification,  // 播报中：停止播报
        kClearPreview,      // 有常驻预览图：清除预览图
        kStartListening,    // 空闲/说话中：进入聆听（与 BOOT 键、唤醒词一致）
        kStopListening,     // 聆听中：返回空闲
    };

    // 屏幕点击事件回调（自行判定双击/三击，见 .cc 中的说明）
    static void OnScreenClickEvent(lv_event_t* e);
    // 双击确认窗口到期：执行双击动作；三击到达时会被取消
    static void OnClickTimer(lv_timer_t* timer);
    // 按当前状态决定双击该做什么；返回 kNone 表示双击无事可做
    DoubleClickAction ResolveDoubleClickAction();
    // 执行已确认的双击动作（在 LVGL 任务里调用）
    void RunDoubleClickAction();
    // 取消尚未确认的双击动作（第三击到达/新序列开始）
    void CancelPendingDoubleClick();
    // 递归给已有对象树补上事件冒泡标志，使任意层级的点击都能冒泡到屏幕根节点
    static void EnableEventBubbling(lv_obj_t* obj);
    // 监听新建对象（如聊天消息气泡），动态补上事件冒泡标志
    static void OnChildCreatedEvent(lv_event_t* e);
    // 清除远程下发的常驻预览图（无图时无动作）
    void ClearPreviewImage();
    // 三击：切换主题（light <-> dark）
    void ToggleTheme();
    // 全屏显示公司 logo（按当前主题选择深/浅色版本）
    void ShowCompanyLogo();
    // 背景图（公司 logo）挂在 container_ 里，位于所有控件之下
    void ApplyBottomLayer();
    lv_obj_t* logo_image_ = nullptr;
    // 远程壁纸是否激活；跨 main/LVGL 任务访问，使用原子量
    std::atomic<bool> wallpaper_active_{false};
    // 上一次短按的 tick（0 表示还没有上一次点击）
    uint32_t last_click_tick_ = 0;
    // 当前连续点击次数：1=单击，2=双击，3=三击
    uint8_t click_streak_ = 0;
    // 双击确认定时器：确认不是三击后才执行双击动作
    lv_timer_t* click_timer_ = nullptr;
    // 已判定、尚未执行的双击动作
    DoubleClickAction pending_action_ = DoubleClickAction::kNone;
    // 待清除的预览图快照：若窗口内又下发了新图，不能把新图误清掉
    const LvglImage* pending_clear_target_ = nullptr;
    // 相邻两次点击间隔在该窗口内算同一次连续点击
    static constexpr uint32_t CLICK_WINDOW_MS = 500;
};

#endif // __CUSTOM_LCD_DISPLAY_H__
