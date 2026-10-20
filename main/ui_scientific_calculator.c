#include "ui_scientific_calculator.h"
#include "bsp_board.h"
#include "bsp_theme.h"
#include "esp_2_41ttf.h"
#include "ui_navbar.h"
#include "lvgl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCI_W MIPI_DSI_LCD_H_RES
#define SCI_MAX_INPUT 31

static lv_obj_t *s_screen;
static lv_obj_t *s_display;
static char s_expr[SCI_MAX_INPUT + 1];
static bool s_new_input;
static bool s_degree_mode;

static void scientific_back_cb(lv_event_t *e)
{
    (void)e;
    ui_navbar_back();
}

static void scientific_update(void)
{
    lv_label_set_text(s_display, s_expr[0] ? s_expr : "0");
}

static void scientific_clear(void)
{
    s_expr[0] = 0;
    s_new_input = true;
    scientific_update();
}

static void scientific_append(const char *text)
{
    if (s_new_input) {
        s_expr[0] = 0;
        s_new_input = false;
    }
    size_t used = strlen(s_expr);
    size_t add = strlen(text);
    if (used + add > SCI_MAX_INPUT) return;
    memcpy(s_expr + used, text, add + 1);
    scientific_update();
}

static double scientific_value(void)
{
    if (!s_expr[0]) return 0;
    char *end = NULL;
    double value = strtod(s_expr, &end);
    if (end == s_expr) return 0;
    return value;
}

static void scientific_unary(const char *op)
{
    double value = scientific_value();
    if (strcmp(op, "sin") == 0) value = s_degree_mode ? sin(value * M_PI / 180.0) : sin(value);
    else if (strcmp(op, "cos") == 0) value = s_degree_mode ? cos(value * M_PI / 180.0) : cos(value);
    else if (strcmp(op, "tan") == 0) value = s_degree_mode ? tan(value * M_PI / 180.0) : tan(value);
    else if (strcmp(op, "sqrt") == 0) value = sqrt(value);
    else if (strcmp(op, "log") == 0) value = log10(value);
    else if (strcmp(op, "ln") == 0) value = log(value);
    else if (strcmp(op, "x2") == 0) value *= value;
    snprintf(s_expr, sizeof(s_expr), "%.10g", value);
    s_new_input = true;
    scientific_update();
}

static void scientific_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *text = (const char *)lv_event_get_user_data(e);
    if (!text) return;
    if (strcmp(text, "C") == 0) scientific_clear();
    else if (strcmp(text, "DEG") == 0 || strcmp(text, "RAD") == 0) {
        s_degree_mode = !s_degree_mode;
        lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), s_degree_mode ? "DEG" : "RAD");
    } else if (!strcmp(text, "sin") || !strcmp(text, "cos") || !strcmp(text, "tan") ||
               !strcmp(text, "sqrt") || !strcmp(text, "log") || !strcmp(text, "ln") ||
               !strcmp(text, "x2")) {
        scientific_unary(text);
    } else if (strcmp(text, "=" ) == 0) {
        scientific_update();
    } else {
        scientific_append(text);
    }
}

static void scientific_button(lv_obj_t *parent, const char *text, int row, int col, bool accent)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, 56, 42);
    lv_obj_set_pos(button, 8 + col * 62, 100 + row * 49);
    lv_obj_set_style_bg_color(button, accent ? bsp_theme_colors()->accent : bsp_theme_colors()->card, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 7, LV_PART_MAIN);
    lv_obj_add_event_cb(button, scientific_button_cb, LV_EVENT_CLICKED, (void *)text);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &esp_2, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(label);
}

void ui_scientific_calculator_show(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, bsp_theme_colors()->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "科学计算器");
    lv_obj_set_style_text_font(title, &esp_2, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    s_display = lv_label_create(s_screen);
    lv_label_set_text(s_display, "0");
    lv_obj_set_width(s_display, SCI_W - 16);
    lv_obj_set_style_text_align(s_display, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_display, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_display, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(s_display, LV_ALIGN_TOP_MID, 0, 48);

    static const char *keys[6][4] = {
        {"C", "DEG", "sin", "cos"},
        {"tan", "sqrt", "log", "ln"},
        {"x2", "(", ")", "/"},
        {"7", "8", "9", "*"},
        {"4", "5", "6", "-"},
        {"1", "2", "3", "+"}
    };
    for (int row = 0; row < 6; row++) {
        for (int col = 0; col < 4; col++) {
            scientific_button(s_screen, keys[row][col], row, col,
                              row < 2 || col == 3 || strcmp(keys[row][col], "=") == 0);
        }
    }

    scientific_button(s_screen, "0", 6, 0, false);
    scientific_button(s_screen, ".", 6, 1, false);
    scientific_button(s_screen, "=", 6, 2, true);

    lv_obj_t *back = lv_btn_create(s_screen);
    lv_obj_set_size(back, 52, 36);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -UI_NAVBAR_H - 6);
    lv_obj_add_event_cb(back, scientific_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);

    s_degree_mode = true;
    scientific_clear();
    ui_navbar_push_screen_named(s_screen, false, "科学计算");
}
