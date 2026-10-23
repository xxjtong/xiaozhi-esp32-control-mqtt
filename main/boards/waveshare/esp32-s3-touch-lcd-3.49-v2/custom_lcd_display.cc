#include "custom_lcd_display.h"

#include "lcd_display.h"
#include "lvgl_theme.h"

#include <vector>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include "assets/lang_config.h"
#include <cstring>
#include "settings.h"

#include "esp_lcd_panel_io.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "config.h"

#include "board.h"
#include "application.h"
#include "device_state.h"
#include "company_logo.h"

#define TAG "CustomLcdDisplay"

static SemaphoreHandle_t trans_done_sem = NULL;
static uint16_t *trans_buf_1;

#if (DISPLAY_ROTATION_90 == true)
static uint16_t *dest_map;
#endif


bool CustomLcdDisplay::lvgl_port_flush_io_ready_callback(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    BaseType_t taskAwake = pdFALSE;
    lv_display_t *disp_drv = (lv_display_t *)user_ctx;
    assert(disp_drv != NULL);
    if (trans_done_sem) {
        xSemaphoreGiveFromISR(trans_done_sem, &taskAwake);
    }
    return false;
}

void CustomLcdDisplay::lvgl_port_flush_callback(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map) {
    assert(drv != NULL);
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(drv);
    assert(panel_handle != NULL);

    lv_draw_sw_rgb565_swap(color_map, lv_area_get_width(area) * lv_area_get_height(area));

#if (DISPLAY_ROTATION_90 == true)
    lv_display_rotation_t rotation = lv_display_get_rotation(drv);
    lv_area_t rotated_area;
    if(rotation != LV_DISPLAY_ROTATION_0) {
        lv_color_format_t cf = lv_display_get_color_format(drv);
        /*Calculate the position of the rotated area*/
        rotated_area = *area;
        lv_display_rotate_area(drv, &rotated_area);
        /*Calculate the source stride (bytes in a line) from the width of the area*/
        uint32_t src_stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), cf);
        /*Calculate the stride of the destination (rotated) area too*/
        uint32_t dest_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&rotated_area), cf);
        /*Have a buffer to store the rotated area and perform the rotation*/

        int32_t src_w = lv_area_get_width(area);
        int32_t src_h = lv_area_get_height(area);
        lv_draw_sw_rotate(color_map, dest_map, src_w, src_h, src_stride, dest_stride, rotation, cf);
        /*Use the rotated area and rotated buffer from now on*/
        area = &rotated_area;
    }
#endif
    const int flush_coun = (LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN);
    const int offgap = (DISPLAY_HEIGHT / flush_coun);
    const int dmalen = (LVGL_DMA_BUFF_LEN / 2);
    int offsetx1 = 0;
    int offsety1 = 0;
    int offsetx2 = DISPLAY_WIDTH;
    int offsety2 = offgap;
#if (DISPLAY_ROTATION_90 == true)
    uint16_t *map = (uint16_t*)dest_map;
#else
    uint16_t *map = (uint16_t*)color_map;
#endif
    xSemaphoreGive(trans_done_sem);

    for(int i = 0; i<flush_coun; i++) {
        xSemaphoreTake(trans_done_sem,portMAX_DELAY);
        memcpy(trans_buf_1,map,LVGL_DMA_BUFF_LEN);
        esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2, offsety2, trans_buf_1);
        offsety1 += offgap;
        offsety2 += offgap;
        map += dmalen;
    }
    xSemaphoreTake(trans_done_sem,portMAX_DELAY);
    lv_disp_flush_ready(drv);
}


CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    trans_done_sem = xSemaphoreCreateBinary();
    trans_buf_1 = (uint16_t *)heap_caps_malloc(LVGL_DMA_BUFF_LEN, MALLOC_CAP_DMA);

    uint32_t buffer_size = 0;
    lv_color_t *buf1 = NULL;
    lvgl_port_lock(0);
    uint8_t color_bytes = lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
    display_ = lv_display_create(width_, height_);
    lv_display_set_flush_cb(display_, lvgl_port_flush_callback);
    buffer_size = width_ * height_;
    buf1 = (lv_color_t *)heap_caps_aligned_alloc(1, buffer_size * color_bytes, MALLOC_CAP_SPIRAM);
#if (DISPLAY_ROTATION_90 == true)
    // 旋转目标缓冲需满足 LVGL 绘制缓冲对齐要求（CONFIG_LV_DRAW_BUF_ALIGN = 4）
    dest_map = (uint16_t *)heap_caps_aligned_alloc(4, buffer_size * color_bytes, MALLOC_CAP_SPIRAM);
    lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_270);
#endif
    lv_display_set_buffers(display_, buf1, NULL, buffer_size * color_bytes, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_user_data(display_, panel_);
    lvgl_port_unlock();

    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lvgl_port_flush_io_ready_callback,
    };
    /* Register done callback */
    esp_lcd_panel_io_register_event_callbacks(panel_io_, &cbs, display_);

    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

// 递归给对象树补上事件冒泡标志：
// LVGL 的事件冒泡是逐级生效的，只有子对象自身带 EVENT_BUBBLE 标志时其事件才会传给父对象，
// 因此需要覆盖从 container_ 往下的所有层级，否则深层对象上的双击到不了 container_。
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
// 否则这些对象上的双击无法冒泡到 container_。
// LV_EVENT_CHILD_CREATED 会始终冒泡到所有父对象，故注册在 container_ 上即可覆盖整棵树。
void CustomLcdDisplay::OnChildCreatedEvent(lv_event_t* e) {
    auto* child = static_cast<lv_obj_t*>(lv_event_get_param(e));
    if (child != nullptr) {
        lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
}

// 双击屏幕切换主题（light <-> dark）
void CustomLcdDisplay::SetupUI() {
    // 先调用父类的 SetupUI 完成标准 UI 初始化
    LcdDisplay::SetupUI();

    if (container_ == nullptr) {
        return;
    }

    // emoji_box_ / emoji_image_ 只用于显示。空闲时会把全屏 logo 放进 emoji_image_，
    // 而 emoji_box_ 是 LV_SIZE_CONTENT，会被撑满整个屏幕；LVGL 里 lv_obj 默认带
    // LV_OBJ_FLAG_CLICKABLE，这个全屏容器会把双击事件全部吞掉，导致 container_
    // 收不到 LV_EVENT_DOUBLE_CLICKED、主题切换失效。这里去掉可点击标志让触摸穿透。
    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_CLICKABLE);

    // 覆盖已有对象树，并监听后续动态创建的对象
    EnableEventBubbling(container_);
    lv_obj_add_event_cb(container_, OnChildCreatedEvent, LV_EVENT_CHILD_CREATED, nullptr);

    lv_obj_add_event_cb(container_, OnScreenClickEvent, LV_EVENT_SHORT_CLICKED, this);

    // 预览图容器铺满屏幕（为了支持上下拖动），它会吞掉落在图片区域上的点击，
    // 事件也不会冒泡到 container_，所以这里单独挂一份双击判定。
    if (preview_viewport_ != nullptr) {
        lv_obj_add_event_cb(preview_viewport_, OnScreenClickEvent, LV_EVENT_SHORT_CLICKED, this);
    }
    ESP_LOGI(TAG, "Theme switch on double-click enabled");
}

// 双击切换主题（light <-> dark）。
// 注意：不能用 LV_EVENT_DOUBLE_CLICKED —— LVGL 9 把单击/双击/三击做成「按 3 次循环」
// 下发（见 lv_indev.c indev_proc_short_click：streak 1→SINGLE、2→DOUBLE、3→TRIPLE、
// 4→SINGLE…），连续点击时只有第 2、5、8 次才是 DOUBLE_CLICKED，
// 表现为「要连点 3 下才切换一次」。
// 因此这里改用每次点击都会下发的 LV_EVENT_SHORT_CLICKED，自己按时间间隔判定双击。
void CustomLcdDisplay::OnScreenClickEvent(lv_event_t* e) {
    auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) return;

    if (self->last_click_tick_ != 0 &&
        lv_tick_elaps(self->last_click_tick_) <= DOUBLE_CLICK_WINDOW_MS) {
        self->last_click_tick_ = 0;
        self->ToggleTheme();
    } else {
        self->last_click_tick_ = lv_tick_get();
    }
}

void CustomLcdDisplay::ToggleTheme() {
    Theme* current = GetTheme();
    if (current == nullptr) return;

    // 远程下发的图片会常驻屏幕并挡住界面，双击时顺手清掉，恢复正常 UI。
    // （LcdDisplay::SetTheme() 只改样式，不会重建对象树，所以图片不会自己消失）
    if (preview_image_cached_ != nullptr) {
        ESP_LOGI(TAG, "Clearing persistent preview image on double-click");
        SetPreviewImage(nullptr);
        Application::GetInstance().ReportUserEvent("image_cleared", "double_click");
    }

    std::string current_name = current->name();
    std::string next_name = (current_name == "light") ? "dark" : "light";

    LvglTheme* next_theme = LvglThemeManager::GetInstance().GetTheme(next_name);
    if (next_theme == nullptr) return;

    ESP_LOGI(TAG, "Switching theme: %s -> %s", current_name.c_str(), next_name.c_str());
    SetTheme(next_theme);
    Application::GetInstance().ReportUserEvent("theme_changed", next_name);
}

// 空闲/开机时用公司 logo 顶替默认的机器人图标。
// 开机阶段默认表情是 "robot_2"，进入空闲后状态机会下发 "neutral"；
// 但 "neutral" 在 connecting/listening 状态下也会下发，所以只有确实处于
// 空闲状态时才换成 logo，其余状态保持原有表情逻辑。
void CustomLcdDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) {
        showing_logo_ = false;
        LcdDisplay::SetEmotion(emotion);
        return;
    }

    bool is_boot_logo = (strcmp(emotion, "robot_2") == 0);
    bool is_idle_neutral = (strcmp(emotion, "neutral") == 0) &&
                           Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
    if (is_boot_logo || is_idle_neutral) {
        ShowCompanyLogo();
        return;
    }

    showing_logo_ = false;
    LcdDisplay::SetEmotion(emotion);
}

void CustomLcdDisplay::ShowCompanyLogo() {
    if (emoji_image_ == nullptr || emoji_label_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    // 深色主题下黑色文字看不见，换用白色文字版本
    auto* theme = static_cast<LvglTheme*>(GetTheme());
    bool dark = (theme != nullptr && theme->name() == "dark");
    lv_image_set_src(emoji_image_, dark ? &virsical_logo_dark : &virsical_logo_light);
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    showing_logo_ = true;
}

void CustomLcdDisplay::SetTheme(Theme* theme) {
    LcdDisplay::SetTheme(theme);
    // 主题切换后 logo 需要重绘（深/浅色版本不同）
    if (showing_logo_) {
        ShowCompanyLogo();
    }
}
