#include "custom_lcd_display.h"

#include "lcd_display.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include "assets/lang_config.h"
#include <cstring>

#include "board.h"
#include "application.h"
#include "device_state.h"
#include "company_logo.h"
#include "imu_monitor.h"

#define TAG "CustomLcdDisplay"

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy)
    : RgbLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
    // 显示管线（LVGL 初始化、RGB 双缓冲、bounce buffer）全部由 RgbLcdDisplay 完成，
    // 这里只挂手势与 logo，无需自建 flush 回调。
    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

CustomLcdDisplay::~CustomLcdDisplay() {
    if (click_timer_ != nullptr) {
        lv_timer_delete(click_timer_);
        click_timer_ = nullptr;
    }
}

// 递归给对象树补上事件冒泡标志：
// LVGL 的事件冒泡是逐级生效的，只有子对象自身带 EVENT_BUBBLE 标志时其事件才会传给父对象，
// 因此需要覆盖从 container_ 往下的所有层级，否则深层对象上的点击到不了 container_。
void CustomLcdDisplay::EnableEventBubbling(lv_obj_t* obj) {
    uint32_t child_cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t* child = lv_obj_get_child(obj, i);
        if (child == nullptr) {
            continue;
        }
        lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
        EnableEventBubbling(child);
    }
}

// 聊天消息等对象在运行期动态创建，创建时补上冒泡标志，
// 否则这些对象上的点击无法冒泡到 container_。
// LV_EVENT_CHILD_CREATED 会始终冒泡到所有父对象，故注册在 container_ 上即可覆盖整棵树。
void CustomLcdDisplay::OnChildCreatedEvent(lv_event_t* e) {
    auto* child = static_cast<lv_obj_t*>(lv_event_get_param(e));
    if (child != nullptr) {
        lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
}

// 注册点击手势：双击清预览图/停播报、三击切换主题（light <-> dark）
void CustomLcdDisplay::SetupUI() {
    // 父类 SetupUI 自带重复调用保护；这里若已初始化过则直接返回，避免把点击
    // 回调和手势定时器再注册一遍（否则一次点击会被处理两次）。
    if (IsSetupUICalled()) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping gesture setup");
        return;
    }

    // 先调用父类的 SetupUI 完成标准 UI 初始化
    RgbLcdDisplay::SetupUI();

    if (container_ == nullptr) {
        return;
    }

    // 父类 SetupUI 返回后已释放 LVGL 锁；这里继续改对象树/注册回调/创建定时器，
    // 必须重新持有显示锁，避免与 LVGL 任务并发。
    DisplayLockGuard lock(this);

    // emoji_box_ / emoji_image_ 只用于显示。空闲时会把全屏 logo 放进 emoji_image_，
    // 而 emoji_box_ 是 LV_SIZE_CONTENT，会被撑满整个屏幕；LVGL 里 lv_obj 默认带
    // LV_OBJ_FLAG_CLICKABLE，这个全屏容器会把点击事件全部吞掉，导致 container_
    // 收不到点击、清图/切主题失效。这里去掉可点击标志让触摸穿透。
    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_CLICKABLE);

    // 覆盖已有对象树，并监听后续动态创建的对象
    EnableEventBubbling(container_);
    lv_obj_add_event_cb(container_, OnChildCreatedEvent, LV_EVENT_CHILD_CREATED, nullptr);

    lv_obj_add_event_cb(container_, OnScreenClickEvent, LV_EVENT_SHORT_CLICKED, this);

    // 预览图容器铺满屏幕（为了支持上下拖动），它会吞掉落在图片区域上的点击，
    // 事件也不会冒泡到 container_，所以这里单独挂一份点击判定。
    if (preview_viewport_ != nullptr) {
        lv_obj_add_event_cb(preview_viewport_, OnScreenClickEvent, LV_EVENT_SHORT_CLICKED, this);
    }
    // One-shot style timer (paused until the second click) used to distinguish a
    // double-click from a triple-click: the third click cancels the pending clear.
    if (click_timer_ == nullptr) {
        click_timer_ = lv_timer_create(OnClickTimer, CLICK_WINDOW_MS, this);
        if (click_timer_ != nullptr) {
            lv_timer_pause(click_timer_);
        }
    }
    // 建立底层壁纸：屏幕设为透明，让 bottom_layer 的 logo 透出来
    ApplyBottomLayer();
    ShowCompanyLogo();

    ESP_LOGI(TAG, "Double-click clears preview, triple-click switches theme");
}

// 双击清预览图/停播报、三击切换主题（light <-> dark）。
// 注意：不能用 LV_EVENT_DOUBLE_CLICKED / LV_EVENT_TRIPLE_CLICKED —— LVGL 9 把单击/双击/
// 三击做成「按 3 次循环」下发（见 lv_indev.c indev_proc_short_click：streak 1→SINGLE、
// 2→DOUBLE、3→TRIPLE、4→SINGLE…），连续点击时只有第 2、5、8 次才是 DOUBLE_CLICKED。
// 因此这里改用每次点击都会下发的 LV_EVENT_SHORT_CLICKED，自己按时间间隔累计点击次数。
void CustomLcdDisplay::OnScreenClickEvent(lv_event_t* e) {
    auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) return;

    // Touching the screen shakes the board. Stamp the interaction so the IMU does not
    // read the tap (or the double/triple click that follows) as a posture change.
    ImuMonitor::NotifyUserInteraction();

    if (self->last_click_tick_ != 0 &&
        lv_tick_elaps(self->last_click_tick_) <= CLICK_WINDOW_MS) {
        if (self->click_streak_ < 3) {
            ++self->click_streak_;
        }
    } else {
        self->click_streak_ = 1;
        // A new sequence started: cancel any pending double-click clear from the
        // previous gesture (it may not have fired yet on a busy LVGL task).
        self->pending_clear_ = false;
        self->pending_clear_target_ = nullptr;
        if (self->click_timer_ != nullptr) {
            lv_timer_pause(self->click_timer_);
        }
    }
    self->last_click_tick_ = lv_tick_get();

    if (self->click_streak_ == 2) {
        // While a broadcast is playing the double-click means "stop playing". It must
        // not also clear the preview image, and the streak is consumed so a third tap
        // cannot switch the theme right after stopping playback.
        if (Application::GetInstance().GetDeviceState() == kDeviceStateNotifying) {
            self->click_streak_ = 0;
            self->last_click_tick_ = 0;
            self->pending_clear_ = false;
            self->pending_clear_target_ = nullptr;
            if (self->click_timer_ != nullptr) {
                lv_timer_pause(self->click_timer_);
            }
            ESP_LOGI(TAG, "Double-click stops notification playback");
            // StopNotificationByUser() must run on the main task; report the event
            // there too so the state it carries matches what really happened (the
            // audio may have ended between the gesture and this callback).
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() != kDeviceStateNotifying) {
                    return;
                }
                app.ReportUserEvent("notify_stopped", "double_click");
                app.StopNotificationByUser();
            });
            return;
        }

        // Wait for the window to expire before clearing: a third click within the
        // window turns this into a triple-click and must cancel the clear.
        {
            DisplayLockGuard lock(self);
            self->pending_clear_ = self->preview_image_cached_ != nullptr;
            self->pending_clear_target_ = self->preview_image_cached_.get();
        }
        if (self->pending_clear_ && self->click_timer_ != nullptr) {
            lv_timer_reset(self->click_timer_);
            lv_timer_resume(self->click_timer_);
        } else {
            self->ClearPreviewImage();
        }
    } else if (self->click_streak_ == 3) {
        // Consume the streak so a fourth click starts a new sequence.
        self->click_streak_ = 0;
        self->last_click_tick_ = 0;
        self->pending_clear_ = false;
        self->pending_clear_target_ = nullptr;
        if (self->click_timer_ != nullptr) {
            lv_timer_pause(self->click_timer_);
        }
        self->ToggleTheme();
    }
}

void CustomLcdDisplay::OnClickTimer(lv_timer_t* timer) {
    auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
    if (self == nullptr) {
        return;
    }
    lv_timer_pause(timer);
    self->click_streak_ = 0;
    self->last_click_tick_ = 0;
    // A new image may have arrived inside the double-click window; only clear the
    // exact image the user double-clicked on.
    {
        DisplayLockGuard lock(self);
        if (self->pending_clear_ &&
            self->preview_image_cached_.get() == self->pending_clear_target_) {
            self->ClearPreviewImage();
        }
    }
    self->pending_clear_ = false;
    self->pending_clear_target_ = nullptr;
}

void CustomLcdDisplay::ClearPreviewImage() {
    // The LVGL task (gesture timer/event) and the main task (remote set_image) can
    // both touch preview_image_cached_; serialize the check with SetPreviewImage().
    DisplayLockGuard lock(this);
    if (preview_image_cached_ == nullptr) {
        return;
    }
    // 远程下发的图片会常驻屏幕并挡住界面，双击时清除，恢复正常 UI。
    ESP_LOGI(TAG, "Clearing persistent preview image on double-click");
    SetPreviewImage(nullptr);
    if (preview_image_cached_ != nullptr) {
        // UI was not ready (or the display rejected the clear); don't claim success.
        ESP_LOGW(TAG, "Failed to clear preview image");
        return;
    }
    Application::GetInstance().ReportUserEvent("image_cleared", "double_click");
}

void CustomLcdDisplay::ToggleTheme() {
    Theme* current = GetTheme();
    if (current == nullptr) return;

    std::string current_name = current->name();
    std::string next_name = (current_name == "light") ? "dark" : "light";

    LvglTheme* next_theme = LvglThemeManager::GetInstance().GetTheme(next_name);
    if (next_theme == nullptr) return;

    ESP_LOGI(TAG, "Switching theme: %s -> %s", current_name.c_str(), next_name.c_str());
    SetTheme(next_theme);
    Application::GetInstance().ReportUserEvent("theme_changed", next_name);
}

// 背景图与表情现在是"同时存在"的关系：
//   - logo 挂在 bottom_layer，常驻，任何状态都不动它；
//   - 表情完全交回基类处理（默认层、默认位置），不再和 logo 抢同一个控件。
// 只有开机（robot_2）和空闲（idle + neutral）这两种"没有意义表情"的状态才把
// emoji 容器整个隐藏，让画面只剩 logo。
void CustomLcdDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) {
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    bool is_boot_logo = (strcmp(emotion, "robot_2") == 0);
    bool is_idle_neutral = (strcmp(emotion, "neutral") == 0) &&
                           Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
    if (is_boot_logo || is_idle_neutral) {
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (emoji_box_ != nullptr) {
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    RgbLcdDisplay::SetEmotion(emotion);
}

// 背景图挂在 container_ 里，不用 bottom_layer + 透明屏幕。
//
// 原因：屏幕透明会让每次重绘都做一次全屏 alpha 合成，实测把 RGB 刷新余量吃掉，
// 出现约 8 秒一次的闪屏（同一块屏在 bounce buffer 加到 40 行后才稳定）。
// 而 container_ 本来就是 LcdDisplay 里的"背景容器"（全屏、创建最早、位于所有 UI 之下），
// 把 logo 放进去就能得到同样的"壁纸"效果，且整条链路保持不透明、零合成开销。
void CustomLcdDisplay::ApplyBottomLayer() {
    auto* theme = static_cast<LvglTheme*>(GetTheme());
    if (container_ != nullptr) {
        lv_obj_set_style_bg_color(container_,
                                  theme != nullptr ? theme->background_color() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    }
    // 屏幕保持不透明，避免引入全屏合成
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
}

void CustomLcdDisplay::ShowCompanyLogo() {
    DisplayLockGuard lock(this);
    if (logo_image_ == nullptr) {
        if (container_ == nullptr) {
            return;
        }
        // container_ 创建早于 emoji_box_ / top_bar_，因此 logo 天然位于所有控件之下
        logo_image_ = lv_image_create(container_);
        lv_obj_remove_flag(logo_image_, LV_OBJ_FLAG_CLICKABLE);
    }

    // 深色主题下黑色文字看不见，换用白色文字版本
    auto* theme = static_cast<LvglTheme*>(GetTheme());
    bool dark = (theme != nullptr && theme->name() == "dark");
    lv_image_set_src(logo_image_, dark ? &virsical_logo_dark : &virsical_logo_light);
    lv_obj_center(logo_image_);
    lv_obj_remove_flag(logo_image_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::SetTheme(Theme* theme) {
    RgbLcdDisplay::SetTheme(theme);
    // 主题切换会重设屏幕背景与配色，底层壁纸需要跟着重贴
    ApplyBottomLayer();
    ShowCompanyLogo();
}
