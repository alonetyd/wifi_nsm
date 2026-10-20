#include "ui_calculator.h"
#include "bsp_board.h"
#include "bsp_theme.h"
#include "esp_2_41ttf.h"
#include "ui_navbar.h"
#include "lvgl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CALC_W MIPI_DSI_LCD_H_RES
#define CALC_H MIPI_DSI_LCD_V_RES
#define CALC_MAX_INPUT 31

static lv_obj_t *s_screen;
static lv_obj_t *s_display;

static void calculator_back_cb(lv_event_t *e)
{
    (void)e;
    ui_navbar_back();
}
static char s_expr[CALC_MAX_INPUT + 1];
static double s_value;
static char s_pending_op;
static bool s_has_value;
static bool s_new_input;

static void calc_update_display(void)
{
    if (s_expr[0]) {
        lv_label_set_text(s_display, s_expr);
        return;
    }
    char text[32];
    snprintf(text, sizeof(text), "%.10g", s_value);
    lv_label_set_text(s_display, text);
}

static void calc_clear(void)
{
    s_expr[0] = 0;
    s_value = 0;
    s_pending_op = 0;
    s_has_value = false;
    s_new_input = true;
    calc_update_display();
}

static double calc_expr_value(void)
{
    if (s_expr[0] == 0) return s_value;
    return strtod(s_expr, NULL);
}

static void calc_append(const char *text)
{
    if (s_new_input) {
        s_expr[0] = 0;
        s_new_input = false;
    }
    size_t used = strlen(s_expr);
    size_t add = strlen(text);
    if (used + add > CALC_MAX_INPUT) return;
    if (strcmp(text, ".") == 0 && strchr(s_expr, '.')) return;
    memcpy(s_expr + used, text, add + 1);
    calc_update_display();
}

static void calc_operator(char op)
{
    double current = calc_expr_value();
    if (s_has_value) {
        switch (s_pending_op) {
            case '+': s_value += current; break;
            case '-': s_value -= current; break;
            case '*': s_value *= current; break;
            case '/': if (current != 0) s_value /= current; break;
            default: s_value = current; break;
        }
    } else {
        s_value = current;
        s_has_value = true;
    }
    s_pending_op = op;
    s_expr[0] = 0;
    s_new_input = true;
    calc_update_display();
}

static void calc_equal(void)
{
    if (!s_has_value) return;
    calc_operator(0);
    s_pending_op = 0;
    s_has_value = false;
    s_new_input = true;
    calc_update_display();
}

static void calc_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *text = (const char *)lv_event_get_user_data(e);
    if (!text) return;
    if (strcmp(text, "C") == 0) calc_clear();
    else if (strcmp(text, "=") == 0) calc_equal();
    else if (strlen(text) == 1 && strchr("+-*/", text[0])) calc_operator(text[0]);
    else calc_append(text);
}

static lv_obj_t *calc_button(lv_obj_t *parent, const char *text, int row, int col, bool accent)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, 56, 48);
    lv_obj_set_pos(button, 8 + col * 62, 102 + row * 56);
    lv_obj_set_style_bg_color(button, accent ? bsp_theme_colors()->accent : bsp_theme_colors()->card, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(button, calc_button_cb, LV_EVENT_CLICKED, (void *)text);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &esp_2, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

void ui_calculator_show(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, bsp_theme_colors()->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "计算器");
    lv_obj_set_style_text_font(title, &esp_2, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    s_display = lv_label_create(s_screen);
    lv_label_set_text(s_display, "0");
    lv_obj_set_width(s_display, CALC_W - 16);
    lv_obj_set_style_text_align(s_display, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_display, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_display, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(s_display, LV_ALIGN_TOP_MID, 0, 52);

    static const char *keys[5][4] = {
        {"C", "/", "*", "-"},
        {"7", "8", "9", "+"},
        {"4", "5", "6", "="},
        {"1", "2", "3", "."},
        {"0", "00", "", ""}
    };
    int row = 0;
    for (row = 0; row < 5; row++) {
        for (int col = 0; col < 4; col++) {
            if (keys[row][col][0] == 0) continue;
            calc_button(s_screen, keys[row][col], row, col,
                        strchr("C/*-+=", keys[row][col][0]) != NULL);
        }
    }

    lv_obj_t *back = lv_btn_create(s_screen);
    lv_obj_set_size(back, 52, 38);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -UI_NAVBAR_H - 8);
    lv_obj_add_event_cb(back, calculator_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);

    calc_clear();
    ui_navbar_push_screen_named(s_screen, false, "计算器");
}
