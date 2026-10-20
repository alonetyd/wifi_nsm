#include "ui_apps.h"
#include "ui_games.h"
#include "ui_music_player.h"
#include "ui_file_manager.h"
#include "ui_settings.h"
#include "game_snake.h"
#include "game_2048.h"
#include "game_gomoku.h"
#include "ui_calculator.h"
#include "ui_scientific_calculator.h"
#include "ui_function_plotter.h"
#include "ui_stopwatch.h"
#include "go_game.h"
#include "ui_navbar.h"
#include "paoku_game.h"

#include "bsp_board.h"
#include "bsp_theme.h"
#include "esp_2_41ttf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

#define APP_W MIPI_DSI_LCD_H_RES
#define APP_H MIPI_DSI_LCD_V_RES
#define APP_NAV_H 52

static lv_obj_t *s_apps_scr;
static lv_obj_t *s_swipe_obj;
static bool s_swipe_pressed;
static lv_point_t s_swipe_start;

static lv_color_t app_bg(void) { return bsp_theme_colors()->bg; }
static lv_color_t app_card(void) { return bsp_theme_colors()->card; }
static lv_color_t app_accent(void) { return bsp_theme_colors()->accent; }

static void app_back(lv_event_t *e)
{
    (void)e;
    ui_navbar_back();
}

static void swipe_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_indev_get_point(indev, &s_swipe_start);
    s_swipe_pressed = true;
}

static void swipe_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_swipe_pressed) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if (s_swipe_start.y - point.y > 35 &&
        s_swipe_start.y >= APP_H - 110 &&
        abs(point.x - s_swipe_start.x) < 80) {
        s_swipe_pressed = false;
        ui_apps_show();
    }
}

static void swipe_released(lv_event_t *e)
{
    (void)e;
    s_swipe_pressed = false;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             lv_event_cb_t cb, void *user_data,
                             lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, app_card(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &esp_2, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    return btn;
}

static void app_item_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    switch (id) {
        case 0: game_snake_show(); break;
        case 1: game_2048_show(); break;
        case 2: game_gomoku_show(); break;
        case 3: game_go_show(); break;
        case 4: ui_music_player_show(); break;
        case 5: ui_file_manager_show(); break;
        case 6: ui_settings_show(); break;
        case 7: ui_calculator_show(); break;
        case 8: ui_scientific_calculator_show(); break;
        case 9: ui_function_plotter_show(); break;
        case 10: ui_stopwatch_show(); break;
        case 11: paoku_game_start(); break;
        default: break;
    }
}

static void create_app_item(lv_obj_t *parent, const char *icon,
                            const char *name, int id)
{
    lv_obj_t *item = lv_btn_create(parent);
    lv_obj_set_size(item, 106, 116);
    lv_obj_set_style_bg_color(item, app_card(), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(item, 14, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_shadow_width(item, 0, 0);
    lv_obj_add_event_cb(item, app_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);

    lv_obj_t *icon_label = lv_label_create(item);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon_label, app_accent(), 0);
    lv_obj_align(icon_label, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *name_label = lv_label_create(item);
    lv_label_set_text(name_label, name);
    lv_obj_set_style_text_font(name_label, &esp_2, 0);
    lv_obj_set_style_text_color(name_label, lv_color_white(), 0);
    lv_obj_align(name_label, LV_ALIGN_BOTTOM_MID, 0, -14);
}

void ui_apps_init(void)
{
    if (s_apps_scr) return;
    s_apps_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_apps_scr, app_bg(), 0);
    lv_obj_set_style_bg_opa(s_apps_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_apps_scr, 0, 0);
    lv_obj_clear_flag(s_apps_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_apps_scr);
    lv_label_set_text(title, "应用");
    lv_obj_set_style_text_font(title, &esp_2, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *grid = lv_obj_create(s_apps_scr);
    lv_obj_set_size(grid, APP_W - 24, APP_H - 160);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 4, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    /* 主轴居中: 每行内的应用卡片水平居中, 避免偏左 */
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    create_app_item(grid, "S", "贪吃蛇", 0);
    create_app_item(grid, "2048", "2048", 1);
    create_app_item(grid, "G", "五子棋", 2);
    create_app_item(grid, "围", "围棋", 3);
    create_app_item(grid, LV_SYMBOL_AUDIO, "音乐", 4);
    create_app_item(grid, LV_SYMBOL_LIST, "文件", 5);
    create_app_item(grid, LV_SYMBOL_SETTINGS, "设置", 6);
    create_app_item(grid, "=", "计算器", 7);
    create_app_item(grid, "f", "科学计算", 8);
    create_app_item(grid, "fx", "函数绘图", 9);
    create_app_item(grid, "T", "秒表", 10);
    create_app_item(grid, "R", "跑酷", 11);

    lv_obj_t *hint = lv_label_create(s_apps_scr);
    lv_label_set_text(hint, "从底部向上滑动打开应用");
    lv_obj_set_style_text_font(hint, &esp_2, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -APP_NAV_H - 12);

    lv_obj_t *back = make_button(s_apps_scr, LV_SYMBOL_LEFT, app_back, NULL, 52, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 16);
}

void ui_apps_show(void)
{
    if (!s_apps_scr) ui_apps_init();
    ui_navbar_push_screen_named(s_apps_scr, true, "应用中心");
}

void ui_apps_add_swipe_trigger(lv_obj_t *desktop)
{
    if (!desktop || s_swipe_obj) return;
    s_swipe_obj = lv_obj_create(desktop);
    lv_obj_remove_style_all(s_swipe_obj);
    lv_obj_set_size(s_swipe_obj, APP_W, 96);
    lv_obj_set_pos(s_swipe_obj, 0, APP_H - 96);
    lv_obj_add_flag(s_swipe_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_swipe_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_swipe_obj, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_swipe_obj, swipe_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_swipe_obj, swipe_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_swipe_obj, swipe_released, LV_EVENT_RELEASED, NULL);
}
