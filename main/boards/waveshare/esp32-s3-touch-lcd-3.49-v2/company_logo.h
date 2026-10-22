#ifndef COMPANY_LOGO_H
#define COMPANY_LOGO_H

#include <lvgl.h>

// 公司 logo（VIRSICAL）全屏图，640x172、透明背景。
// 由 main/boards/waveshare/esp32-s3-touch-lcd-3.49-v2/virsical_s.png 等比例缩放
// （占 525x132，水平居中、垂直中心在 y=103，避开顶部状态栏）后生成的 PNG 数据。
// light：浅色主题用（黑色文字）；dark：深色主题用（文字改白色，彩色图形保留）。
extern const lv_image_dsc_t virsical_logo_light;
extern const lv_image_dsc_t virsical_logo_dark;

#endif  // COMPANY_LOGO_H
