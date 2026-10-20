#include "ui_function_plotter.h"
#include "bsp_board.h"
#include "bsp_theme.h"
#include "esp_2_41ttf.h"
#include "ui_navbar.h"
#include "lvgl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLOT_W MIPI_DSI_LCD_H_RES
#define PLOT_H 340
#define PLOT_LEFT 22
#define PLOT_TOP 84
#define PLOT_RIGHT 8
#define PLOT_BOTTOM 18
#define PLOT_POINTS 80

static lv_obj_t *s_screen;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_series;
static lv_obj_t *s_expr_label;
static double s_x_min = -10.0;
static double s_x_max = 10.0;
static char s_expression[32] = "sin(x)";

static void plot_back_cb(lv_event_t *e)
{
    (void)e;
    ui_navbar_back();
}

static double plot_value(double x)
{
    if (strcmp(s_expression, "cos(x)") == 0) return cos(x);
    if (strcmp(s_expression, "x*x") == 0) return x * x;
    if (strcmp(s_expression, "abs(x)") == 0) return fabs(x);
    if (strcmp(s_expression, "sin(x)") == 0) return sin(x);
    return sin(x);
}

static void plot_refresh(void)
{
    if (!s_chart || !s_series) return;
    int y_min = -10;
    int y_max = 10;
    if (strcmp(s_expression, "x*x") == 0) {
        y_min = 0;
        y_max = 100;
    }
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, y_min, y_max);
    for (int i = 0; i < PLOT_POINTS; i++) {
        double ratio = (double)i / (double)(PLOT_POINTS - 1);
        double x = s_x_min + (s_x_max - s_x_min) * ratio;
        double y = plot_value(x);
        int value = (int)lround(y);
        if (value < y_min) value = y_min;
        if (value > y_max) value = y_max;
        lv_chart_set_series_value_by_id(s_chart, s_series, (uint32_t)i, value);
    }
    lv_chart_refresh(s_chart);
    char text[64];
    snprintf(text, sizeof(text), "f(x)=%s  [%g,%g]", s_expression, s_x_min, s_x_max);
    lv_label_set_text(s_expr_label, text);
}

static void plot_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *text = (const char *)lv_event_get_user_data(e);
    if (!text) return;
    if (strcmp(text, "sin(x)") == 0 || strcmp(text, "cos(x)") == 0 ||
        strcmp(text, "x*x") == 0 || strcmp(text, "abs(x)") == 0) {
        strncpy(s_expression, text, sizeof(s_expression) - 1);
        s_expression[sizeof(s_expression) - 1] = 0;
        plot_refresh();
    } else if (strcmp(text, "+") == 0) {
        s_x_min -= 2.0;
        s_x_max += 2.0;
        plot_refresh();
    } else if (strcmp(text, "-") == 0) {
        if (s_x_max - s_x_min > 4.0) {
            s_x_min += 2.0;
            s_x_max -= 2.0;
            plot_refresh();
        }
    } else if (strcmp(text, "0") == 0) {
        s_x_min = -10.0;
        s_x_max = 10.0;
        plot_refresh();
    }
}

static void plot_make_button(lv_obj_t *parent, const char *text, int x, int y)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, 58, 34);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, bsp_theme_colors()->card, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 7, LV_PART_MAIN);
    lv_obj_add_event_cb(button, plot_button_cb, LV_EVENT_CLICKED, (void *)text);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &esp_2, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(label);
}

void ui_function_plotter_show(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, bsp_theme_colors()->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "函数绘制器");
    lv_obj_set_style_text_font(title, &esp_2, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    s_expr_label = lv_label_create(s_screen);
    lv_obj_set_width(s_expr_label, PLOT_W - 16);
    lv_obj_set_style_text_align(s_expr_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_expr_label, bsp_theme_colors()->accent, LV_PART_MAIN);
    lv_obj_align(s_expr_label, LV_ALIGN_TOP_MID, 0, 48);

    s_chart = lv_chart_create(s_screen);
    lv_obj_set_size(s_chart, PLOT_W - 12, PLOT_H);
    lv_obj_set_pos(s_chart, 6, PLOT_TOP);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, PLOT_POINTS);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, -10, 10);
    lv_chart_set_div_line_count(s_chart, 5, 5);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x304050), LV_PART_MAIN);
    s_series = lv_chart_add_series(s_chart, bsp_theme_colors()->accent, LV_CHART_AXIS_PRIMARY_Y);

    plot_make_button(s_screen, "sin(x)", 8, 438);
    plot_make_button(s_screen, "cos(x)", 70, 438);
    plot_make_button(s_screen, "x*x", 132, 438);
    plot_make_button(s_screen, "abs(x)", 194, 438);
    plot_make_button(s_screen, "+", 8, 480);
    plot_make_button(s_screen, "-", 70, 480);
    plot_make_button(s_screen, "0", 132, 480);

    lv_obj_t *back = lv_btn_create(s_screen);
    lv_obj_set_size(back, 52, 36);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -UI_NAVBAR_H - 6);
    lv_obj_add_event_cb(back, plot_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);

    plot_refresh();
    ui_navbar_push_screen_named(s_screen, false, "函数绘图");
}
