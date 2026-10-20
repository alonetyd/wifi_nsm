#ifndef GAME_CANVAS_COMPAT_H
#define GAME_CANVAS_COMPAT_H

#include "lvgl.h"
#include <stdlib.h>

#ifndef LV_TEXT_FLAG_NONE
#define LV_TEXT_FLAG_NONE ((lv_text_flag_t)0)
#endif

static inline void game_canvas_draw_rect(lv_obj_t *canvas, int x, int y, int w, int h,
                                          const lv_draw_rect_dsc_t *dsc)
{
    if (!canvas || !dsc || w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            bool border = dsc->border_width > 0 &&
                          (xx < x + dsc->border_width || xx >= x + w - dsc->border_width ||
                           yy < y + dsc->border_width || yy >= y + h - dsc->border_width);
            if (border) {
                lv_canvas_set_px(canvas, xx, yy, dsc->border_color, dsc->border_opa);
            } else if (dsc->bg_opa != LV_OPA_TRANSP) {
                lv_canvas_set_px(canvas, xx, yy, dsc->bg_color, dsc->bg_opa);
            }
        }
    }
}

static inline void game_canvas_draw_line(lv_obj_t *canvas, const lv_point_t *points, uint32_t point_cnt,
                                          const lv_draw_line_dsc_t *dsc)
{
    if (!canvas || !points || !dsc || point_cnt < 2) return;
    for (uint32_t i = 1; i < point_cnt; i++) {
        int x0 = points[i - 1].x;
        int y0 = points[i - 1].y;
        int x1 = points[i].x;
        int y1 = points[i].y;
        int dx = abs(x1 - x0);
        int sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0);
        int sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            for (int oy = -(int)dsc->width / 2; oy <= (int)dsc->width / 2; oy++) {
                for (int ox = -(int)dsc->width / 2; ox <= (int)dsc->width / 2; ox++) {
                    lv_canvas_set_px(canvas, x0 + ox, y0 + oy, dsc->color, dsc->opa);
                }
            }
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

static inline void game_canvas_draw_text(lv_obj_t *canvas, int x, int y, int w,
                                          const lv_draw_label_dsc_t *dsc, const char *text)
{
    if (!canvas || !dsc || !text || w <= 0) return;
    int h = dsc->font ? lv_font_get_line_height(dsc->font) : 16;
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_area_t area = {x, y, x + w - 1, y + h - 1};
    lv_draw_label_dsc_t draw_dsc = *dsc;
    draw_dsc.text = text;
    lv_draw_label(&layer, &draw_dsc, &area);
    lv_canvas_finish_layer(canvas, &layer);
    /* finish_layer 的 dispatch 可能是异步的 (尤其开启 DMA2D 时),
     * 若 buffer 是临时/复用的, 必须等待渲染真正完成, 否则内容错乱 (糊) */
    lv_draw_wait_for_finish();
}

/* 实心圆 (中点画圆, 水平逐行填充), 完全同步逐像素写入 */
static inline void game_canvas_draw_circle(lv_obj_t *canvas, int cx, int cy, int r,
                                            lv_color_t color, lv_opa_t opa)
{
    if (!canvas || r <= 0) return;
    int x = 0;
    int y = r;
    int d = 3 - 2 * r;
    while (x <= y) {
        for (int i = cx - x; i <= cx + x; i++) {
            lv_canvas_set_px(canvas, i, cy - y, color, opa);
            lv_canvas_set_px(canvas, i, cy + y, color, opa);
        }
        for (int i = cx - y; i <= cx + y; i++) {
            lv_canvas_set_px(canvas, i, cy - x, color, opa);
            lv_canvas_set_px(canvas, i, cy + x, color, opa);
        }
        x++;
        if (d < 0) {
            d += 4 * x + 6;
        } else {
            y--;
            d += 4 * (x - y) + 10;
        }
    }
}

/* 圆环: 先画半径 r 的实心圆, 再用 fill_color 画半径 r-s 的内圆, 形成 s 像素宽的环 */
static inline void game_canvas_draw_circle_ring(lv_obj_t *canvas, int cx, int cy, int r,
                                                 int s, lv_color_t color, lv_color_t fill_color,
                                                 lv_opa_t opa)
{
    if (!canvas || r <= 0 || s <= 0) return;
    game_canvas_draw_circle(canvas, cx, cy, r, color, opa);
    int inner = r - s;
    if (inner > 0) {
        game_canvas_draw_circle(canvas, cx, cy, inner, fill_color, opa);
    }
}

#define lv_canvas_draw_rect game_canvas_draw_rect
#define lv_canvas_draw_line game_canvas_draw_line
#define lv_canvas_draw_text game_canvas_draw_text
#define lv_canvas_draw_circle game_canvas_draw_circle

#endif
