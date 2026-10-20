#include "ui_stopwatch.h"
#include "bsp_board.h"
#include "bsp_theme.h"
#include "esp_2_41ttf.h"
#include "ui_navbar.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdint.h>

static lv_obj_t *s_screen;
static lv_obj_t *s_time_label;
static lv_obj_t *s_state_label;
static uint32_t s_elapsed_ms;
static uint32_t s_started_ms;
static bool s_running;
static lv_timer_t *s_timer;

static uint32_t now_ms(void)
{
    return lv_tick_get();
}

static void stopwatch_refresh(void)
{
    uint32_t elapsed = s_elapsed_ms;
    if (s_running) elapsed += now_ms() - s_started_ms;
    char text[24];
    snprintf(text, sizeof(text), "%02lu:%02lu.%01lu",
             (unsigned long)(elapsed / 60000U),
             (unsigned long)((elapsed / 1000U) % 60U),
             (unsigned long)((elapsed % 1000U) / 100U));
    lv_label_set_text(s_time_label, text);
}

static void stopwatch_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    stopwatch_refresh();
}

static void stopwatch_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *command = (const char *)lv_event_get_user_data(e);
    if (!command) return;
    if (command[0] == 's') {
        if (s_running) {
            s_elapsed_ms += now_ms() - s_started_ms;
            s_running = false;
            lv_label_set_text(s_state_label, "已暂停");
        } else {
            s_started_ms = now_ms();
            s_running = true;
            lv_label_set_text(s_state_label, "计时中");
        }
    } else {
        s_elapsed_ms = 0;
        s_started_ms = now_ms();
        s_running = false;
        lv_label_set_text(s_state_label, "已复位");
    }
    stopwatch_refresh();
}

static void stopwatch_back_cb(lv_event_t *e)
{
    (void)e;
    ui_navbar_back();
}

static void make_button(lv_obj_t *parent, const char *text, const char *command,
                        int x, int y)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, 100, 48);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, bsp_theme_colors()->card, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(button, stopwatch_button_cb, LV_EVENT_CLICKED, (void *)command);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &esp_2, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(label);
}

void ui_stopwatch_show(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, bsp_theme_colors()->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "秒表");
    lv_obj_set_style_text_font(title, &esp_2, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    s_time_label = lv_label_create(s_screen);
    lv_label_set_text(s_time_label, "00:00.0");
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_time_label, bsp_theme_colors()->accent, LV_PART_MAIN);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 90);

    s_state_label = lv_label_create(s_screen);
    lv_label_set_text(s_state_label, "已复位");
    lv_obj_set_style_text_font(s_state_label, &esp_2, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0x9ca3af), LV_PART_MAIN);
    lv_obj_align(s_state_label, LV_ALIGN_TOP_MID, 0, 138);

    make_button(s_screen, "开始/暂停", "s", 24, 210);
    make_button(s_screen, "复位", "r", 138, 210);

    lv_obj_t *back = lv_btn_create(s_screen);
    lv_obj_set_size(back, 52, 38);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -UI_NAVBAR_H - 8);
    lv_obj_add_event_cb(back, stopwatch_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);

    s_elapsed_ms = 0;
    s_started_ms = now_ms();
    s_running = false;
    s_timer = lv_timer_create(stopwatch_timer_cb, 50, NULL);
    stopwatch_refresh();
    ui_navbar_push_screen_named(s_screen, false, "秒表");
}
