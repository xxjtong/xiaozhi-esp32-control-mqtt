#ifndef COMPANY_LOGO_H
#define COMPANY_LOGO_H

#include <lvgl.h>

// 公司 logo（VIRSICAL）全屏图，透明背景。
// 由 scripts/gen_company_logo.py 从本目录 assets/ 下的 PNG 生成，请勿手改。
// light：浅色主题用（黑色文字）；dark：深色主题用（文字改白色，彩色图形保留）。
extern const lv_image_dsc_t virsical_logo_light;
extern const lv_image_dsc_t virsical_logo_dark;

#endif  // COMPANY_LOGO_H
