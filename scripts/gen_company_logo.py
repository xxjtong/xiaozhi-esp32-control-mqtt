#!/usr/bin/env python3
"""把公司 logo PNG 转成 LVGL 可用的 C 源文件（company_logo.c/.h）。

用法（以 4B 板为例）：

    python3 scripts/gen_company_logo.py \
        --light main/boards/waveshare/esp32-s3-touch-lcd-4b/assets/virsical_logo_light.png \
        --dark  main/boards/waveshare/esp32-s3-touch-lcd-4b/assets/virsical_logo_dark.png \
        --output main/boards/waveshare/esp32-s3-touch-lcd-4b

PNG 用带透明的调色板格式（P 模式 + transparency），与 3.49-V2 板的做法一致：
设备端用 LV_COLOR_FORMAT_RAW_ALPHA 交给 lodepng 解码，无需额外转换工具链。

源图处理约定（在 2026-10-26 生成 4B 资源时执行过）：
  从 3.49-V2 的 company_logo.c 内嵌 PNG 提取原图（640x172），
  按非透明内容裁剪（525x132），等比缩放到 360 宽（480 屏）后量化为 96 色调色板 PNG。
"""

import argparse
import os
import re

HEADER_TMPL = """#ifndef COMPANY_LOGO_H
#define COMPANY_LOGO_H

#include <lvgl.h>

// 公司 logo（VIRSICAL）全屏图，透明背景。
// 由 scripts/gen_company_logo.py 从本目录 assets/ 下的 PNG 生成，请勿手改。
// light：浅色主题用（黑色文字）；dark：深色主题用（文字改白色，彩色图形保留）。
extern const lv_image_dsc_t {name}_light;
extern const lv_image_dsc_t {name}_dark;

#endif  // COMPANY_LOGO_H
"""


def png_to_c_array(png_path, array_name):
    with open(png_path, "rb") as f:
        data = f.read()
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    body = "\n".join(lines).rstrip(",")
    # PNG 头部信息（宽高）写进注释，方便人工核对
    w = int.from_bytes(data[16:20], "big")
    h = int.from_bytes(data[20:24], "big")
    header = f"static const uint8_t {array_name}[] = {{\n{body}\n}};\n"
    comment = f"// {os.path.basename(png_path)}: {w}x{h}, {len(data)} bytes\n"
    return comment + header, len(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--light", required=True, help="浅色主题 logo PNG（透明背景）")
    ap.add_argument("--dark", required=True, help="深色主题 logo PNG（透明背景）")
    ap.add_argument("--name", default="virsical_logo", help="导出变量名前缀")
    ap.add_argument("--output", required=True, help="输出目录（生成 company_logo.c/.h）")
    args = ap.parse_args()

    os.makedirs(args.output, exist_ok=True)
    light_arr, light_n = png_to_c_array(args.light, f"{args.name}_light_png")
    dark_arr, dark_n = png_to_c_array(args.dark, f"{args.name}_dark_png")

    c = (
        "// 本文件由 scripts/gen_company_logo.py 自动生成（PNG 内嵌 + RAW_ALPHA 交给 lodepng 解码），请勿手改。\n"
        '#include "company_logo.h"\n\n'
        f"// light: {light_n} bytes, dark: {dark_n} bytes\n\n"
        + light_arr
        + "\n"
        + dark_arr
        + f"""
// cf 用 RAW_ALPHA 表示「这里是编码后的 PNG 数据，交给 lodepng 解码」，
// 与项目里 LvglRawImage 的做法一致（见 main/display/lvgl_display/lvgl_image.cc）。

const lv_image_dsc_t {args.name}_light = {{
    .header =
        {{
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RAW_ALPHA,
            .w = 0,
            .h = 0,
        }},
    .data_size = sizeof({args.name}_light_png),
    .data = {args.name}_light_png,
}};

const lv_image_dsc_t {args.name}_dark = {{
    .header =
        {{
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RAW_ALPHA,
            .w = 0,
            .h = 0,
        }},
    .data_size = sizeof({args.name}_dark_png),
    .data = {args.name}_dark_png,
}};
"""
    )

    with open(os.path.join(args.output, "company_logo.c"), "w") as f:
        f.write(c)
    with open(os.path.join(args.output, "company_logo.h"), "w") as f:
        f.write(HEADER_TMPL.format(name=args.name))
    print(f"written: {args.output}/company_logo.c (light {light_n}B, dark {dark_n}B)")
    print(f"written: {args.output}/company_logo.h")


if __name__ == "__main__":
    main()
