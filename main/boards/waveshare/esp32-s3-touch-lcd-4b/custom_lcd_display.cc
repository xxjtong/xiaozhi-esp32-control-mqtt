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

// 注册点击手势：双击（停播报/清预览图/空闲进聆听）、三击切换主题（light <-> dark）
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

    // emoji_box_ / emoji_image_ 只用于显示。emoji_box_ 是 LV_SIZE_CONTENT，
    // 空闲时可能被撑满整个屏幕；LVGL 里 lv_obj 默认带 LV_OBJ_FLAG_CLICKABLE，
    // 这个容器会把点击事件全部吞掉，导致屏幕根节点收不到点击、手势失效。
    // 这里去掉可点击标志让触摸穿透。
    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_CLICKABLE);

    // 覆盖已有对象树，并监听后续动态创建的对象
    EnableEventBubbling(container_);
    lv_obj_add_event_cb(container_, OnChildCreatedEvent, LV_EVENT_CHILD_CREATED, nullptr);

    // 点击判定只注册一次，挂在屏幕根节点上；冒泡让所有子对象（含兄弟对象
    // emoji_box_ / status_bar_ / top_bar_ / bottom_bar_ / preview_viewport_）
    // 的点击都能上浮到这里。注意不能同时还在 container_ / preview_viewport_
    // 上注册——那样一次点击会被判定两次，三击就变成计数 6。
    EnableEventBubbling(lv_screen_active());
    lv_obj_add_event_cb(lv_screen_active(), OnScreenClickEvent, LV_EVENT_SHORT_CLICKED, this);

    // 双击动作要等确认窗口结束才执行：窗口内来了第三击就改判为三击切主题。
    if (click_timer_ == nullptr) {
        click_timer_ = lv_timer_create(OnClickTimer, CLICK_WINDOW_MS, this);
        if (click_timer_ != nullptr) {
            lv_timer_pause(click_timer_);
        }
    }
    // 建立底层壁纸：container_ 背景不透明，logo 挂在 container_ 最底层
    ApplyBottomLayer();
    ShowCompanyLogo();

    ESP_LOGI(TAG, "Gestures: double-click=notify stop/preview clear/listen, triple-click=theme");
}

// 双击清预览图/停播报/进聆听、三击切换主题（light <-> dark）。
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
        // 新序列：上一条双击若还没等到确认窗口结束，先补执行再取消，
        // 避免 LVGL 任务繁忙导致双击动作被第三击吞掉。
        if (self->pending_action_ != DoubleClickAction::kNone) {
            self->RunDoubleClickAction();
        }
        self->CancelPendingDoubleClick();
        self->click_streak_ = 1;
    }
    self->last_click_tick_ = lv_tick_get();

    if (self->click_streak_ == 2) {
        // 第二击只做判定，不立即执行：窗口内若来第三击就改判为三击切主题。
        self->pending_action_ = self->ResolveDoubleClickAction();
        if (self->pending_action_ != DoubleClickAction::kNone && self->click_timer_ != nullptr) {
            lv_timer_reset(self->click_timer_);
            lv_timer_resume(self->click_timer_);
        }
    } else if (self->click_streak_ == 3) {
        // 三击优先：取消待执行的双击动作，只切主题。
        self->CancelPendingDoubleClick();
        self->click_streak_ = 0;
        self->last_click_tick_ = 0;
        self->ToggleTheme();
    }
}

CustomLcdDisplay::DoubleClickAction CustomLcdDisplay::ResolveDoubleClickAction() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    // 播报中双击 = 停止播报（优先级最高，不清图也不进聆听）。
    if (state == kDeviceStateNotifying) {
        return DoubleClickAction::kStopNotification;
    }

    // 有常驻预览图时双击 = 清图；记下当前图片，窗口内若换了新图则不动新图。
    {
        DisplayLockGuard lock(this);
        if (preview_image_cached_ != nullptr) {
            pending_clear_target_ = preview_image_cached_.get();
            return DoubleClickAction::kClearPreview;
        }
        pending_clear_target_ = nullptr;
    }

    // 空闲时双击 = 进入聆听，与 BOOT 键、唤醒词走同一条状态机路径。
    if (state == kDeviceStateIdle) {
        return DoubleClickAction::kStartListening;
    }

    return DoubleClickAction::kNone;
}

void CustomLcdDisplay::CancelPendingDoubleClick() {
    pending_action_ = DoubleClickAction::kNone;
    pending_clear_target_ = nullptr;
    if (click_timer_ != nullptr) {
        lv_timer_pause(click_timer_);
    }
}

void CustomLcdDisplay::RunDoubleClickAction() {
    const DoubleClickAction action = pending_action_;
    pending_action_ = DoubleClickAction::kNone;
    if (action == DoubleClickAction::kNone) {
        pending_clear_target_ = nullptr;
        return;
    }

    if (action == DoubleClickAction::kStopNotification) {
        ESP_LOGI(TAG, "Double-click stops notification playback");
        // 与状态变更/上报都在主任务执行，保证 event 里的 state 是真实值。
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateNotifying) {
                return;
            }
            app.ReportUserEvent("notify_stopped", "double_click");
            app.StopNotificationByUser();
        });
    } else if (action == DoubleClickAction::kClearPreview) {
        // 只清双击时看到的那张图，窗口内新下发的图不能误清。
        DisplayLockGuard lock(this);
        if (pending_clear_target_ != nullptr &&
            preview_image_cached_.get() == pending_clear_target_) {
            ClearPreviewImage();
        }
    } else if (action == DoubleClickAction::kStartListening) {
        ESP_LOGI(TAG, "Double-click: entering listening from idle");
        // StartListening() 只置事件位，但 GetDefaultListeningMode() 读的是主任务
        // 字段；统一丢回主任务执行，和 IMU 触发聆听的路径保持一致。
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateIdle) {
                return;
            }
            app.StartListening(app.GetDefaultListeningMode());
        });
    }

    pending_clear_target_ = nullptr;
}

void CustomLcdDisplay::OnClickTimer(lv_timer_t* timer) {
    auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
    if (self == nullptr) {
        return;
    }
    lv_timer_pause(timer);
    self->click_streak_ = 0;
    self->last_click_tick_ = 0;
    self->RunDoubleClickAction();
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
    // SetEmotion() 由 main 任务在状态切换时调用，也可能被 LVGL 任务调用；
    // 这里对 emoji_box_ 的 add/remove_flag 必须在 LVGL 锁内执行，否则会和
    // LVGL 任务并发修改对象树/堆，实测会卡在 lv_tlsf_free 并触发 10s 看门狗。
    DisplayLockGuard lock(this);

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
    // 同 SetEmotion：远程 set_theme / 三击切主题都可能从 main 任务进入，
    // ApplyBottomLayer() 会直接改 container_/screen 的样式，必须持有 LVGL 锁。
    DisplayLockGuard lock(this);
    RgbLcdDisplay::SetTheme(theme);
    // 主题切换会重设屏幕背景与配色，底层壁纸需要跟着重贴
    ApplyBottomLayer();
    ShowCompanyLogo();
}
