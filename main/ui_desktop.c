/*******************************************************************************
 * 小米手环风格桌面 UI 模块实现 - LVGL 9.x
 *
 * 屏幕: 262×928 (竖屏窄长) RGB565
 * 布局 (从上到下):
 *   状态栏(40) | 表盘区域(888)
 * 控制中心: 从顶部下拉的半透明面板 (覆盖屏幕上半部分)
 * 应用入口: 桌面左滑 / 底部上滑进入应用中心 (无固定应用按钮)
 ******************************************************************************/

#include "ui_desktop.h"
#include "esp_2_41ttf.h"
#include "bsp_board.h"
#include "bsp_brightness.h"
#include "bsp_audio.h"
#include "bsp_theme.h"
#include "ui_apps.h"
#include <stdbool.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ======================== 颜色定义 (使用主题系统) ======================== */
static inline lv_color_t theme_bg(void)         { return bsp_theme_colors()->bg; }
static inline lv_color_t theme_watch_top(void)  { return bsp_theme_colors()->watch_top; }
static inline lv_color_t theme_watch_bot(void)  { return bsp_theme_colors()->watch_bot; }
static inline lv_color_t theme_card(void)       { return bsp_theme_colors()->card; }
static inline lv_color_t theme_accent(void)     { return bsp_theme_colors()->accent; }

#define COLOR_BG          theme_bg()
#define COLOR_WATCH_TOP   theme_watch_top()
#define COLOR_WATCH_BOT   theme_watch_bot()
#define COLOR_CARD        theme_card()
#define COLOR_ACCENT      theme_accent()
#define COLOR_WHITE       lv_color_hex(0xffffff)
#define COLOR_GRAY        lv_color_hex(0x888888)
#define COLOR_TOGGLE_ON   theme_accent()
#define COLOR_TOGGLE_OFF  lv_color_hex(0x333333)

/* ======================== 屏幕尺寸 / 区域坐标 ======================== */
#define SCREEN_W   262
#define SCREEN_H   928

#define NAVBAR_H_DESK   52                          /* 底部导航栏高度 (供布局使用) */
#define STATUS_BAR_H   40                          /* 状态栏高度 */
#define CC_PANEL_H     380                          /* 控制中心面板高度 */
#define TOGGLE_BTN_SIZE 56                          /* 快捷开关按钮尺寸 */

/* ======================== 快捷开关 ID ======================== */
enum {
    TOGGLE_WIFI = 0,
    TOGGLE_BT,
    TOGGLE_MUTE,
    TOGGLE_SETTINGS,
    TOGGLE_COUNT
};

/* ======================== 静态 UI 对象引用 ======================== */
static lv_obj_t *g_desktop_scr = NULL;              /* 桌面屏幕 */
static lv_obj_t *g_cc_mask    = NULL;               /* 控制中心半透明遮罩 */
static lv_obj_t *g_cc_panel   = NULL;               /* 控制中心面板 */

/* 状态栏 */
static lv_obj_t *g_wifi_lbl;                        /* WiFi 图标 */
static lv_obj_t *g_bt_lbl;                          /* 蓝牙图标 */
static lv_obj_t *g_time_lbl;                        /* 状态栏时间 */
static lv_obj_t *g_batt_lbl;                       /* 电池图标 */

/* 表盘 */
static lv_obj_t *g_clock_lbl;                       /* 大号时间 */
static lv_obj_t *g_date_lbl;                        /* 日期星期 */

/* 控制中心 */
static lv_obj_t *g_toggle_btns[TOGGLE_COUNT];       /* 开关按钮 */
static lv_obj_t *g_toggle_icons[TOGGLE_COUNT];      /* 开关图标 */
static bool      g_toggle_states[TOGGLE_COUNT];     /* 开关状态 */
static lv_obj_t *g_brightness_slider;               /* 亮度滑块 */
static lv_obj_t *g_volume_slider;                   /* 音量滑块 */

/* ======================== 静态状态 ======================== */
static bool    g_cc_visible = false;                /* 控制中心是否可见 */
static bool    g_wifi_connected = false;
static bool    g_bt_connected = false;
static bool    g_slider_updating = false;            /* 防止滑块回调递归 */
static uint8_t g_saved_volume = VOLUME_DEFAULT;      /* 静音前保存的音量 */

/* ======================== 子页面入口 (extern) ======================== */
extern void ui_settings_show(void);

/* ======================== 前置声明 ======================== */
static lv_obj_t *make_container(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                lv_coord_t w, lv_coord_t h);
static void create_status_bar(lv_obj_t *p);
static void create_watch_face(lv_obj_t *p);
static void create_control_center(lv_obj_t *parent);
static lv_obj_t *create_toggle(lv_obj_t *parent, const char *icon,
                               const char *name, int id);

static void cb_gesture(lv_event_t *e);
static void cb_toggle(lv_event_t *e);
static void cb_brightness(lv_event_t *e);
static void cb_volume(lv_event_t *e);
static void cb_cc_mask_click(lv_event_t *e);
static void cb_timer(lv_timer_t *t);
static void cb_cc_anim(void *var, int32_t v);
static void cb_cc_anim_done(lv_anim_t *a);

static void update_toggle_style(int idx);
static void update_time_display(void);
static const char *get_weekday_cn(int wday);

/* ========================================================================
 * 辅助函数
 * ======================================================================== */

/* 创建透明无边框容器 (用于各区域) */
static lv_obj_t *make_container(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

/* 获取中文星期 */
static const char *get_weekday_cn(int wday)
{
    static const char *days[] = {
        "星期日", "星期一", "星期二", "星期三",
        "星期四", "星期五", "星期六"
    };
    if (wday >= 0 && wday < 7) return days[wday];
    return "";
}

/* 刷新时间显示 (状态栏 + 表盘) */
static void update_time_display(void)
{
    time_t now = time(NULL);
    struct tm *ti = localtime(&now);
    if (!ti) return;

    char buf[128];

    /* 状态栏时间: HH:MM */
    snprintf(buf, sizeof(buf), "%02d:%02d", ti->tm_hour, ti->tm_min);
    lv_label_set_text(g_time_lbl, buf);

    /* 表盘大号时间: HH:MM */
    lv_label_set_text(g_clock_lbl, buf);

    /* 日期: 2026年7月22日 星期一 */
    snprintf(buf, sizeof(buf), "%d年%d月%d日 %s",
             ti->tm_year + 1900,
             ti->tm_mon + 1,
             ti->tm_mday,
             get_weekday_cn(ti->tm_wday));
    lv_label_set_text(g_date_lbl, buf);
}

/* 更新开关按钮样式 */
static void update_toggle_style(int idx)
{
    if (idx < 0 || idx >= TOGGLE_COUNT) return;
    lv_obj_t *btn = g_toggle_btns[idx];
    if (!btn) return;

    if (g_toggle_states[idx]) {
        lv_obj_set_style_bg_color(btn, COLOR_TOGGLE_ON, 0);
    } else {
        lv_obj_set_style_bg_color(btn, COLOR_TOGGLE_OFF, 0);
    }
}

/* ========================================================================
 * UI 区域创建
 * ======================================================================== */

/* 1. 顶部状态栏 (高40): WiFi | 蓝牙  ……  时间 | 电池 */
static void create_status_bar(lv_obj_t *p)
{
    lv_obj_t *bar = make_container(p, 0, 0, SCREEN_W, STATUS_BAR_H);
    lv_obj_set_style_bg_color(bar, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    /* 左侧: WiFi 图标 */
    g_wifi_lbl = lv_label_create(bar);
    lv_label_set_text(g_wifi_lbl, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(g_wifi_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_wifi_lbl, COLOR_GRAY, 0);
    lv_obj_align(g_wifi_lbl, LV_ALIGN_LEFT_MID, 8, 0);

    /* 左侧: 蓝牙图标 */
    g_bt_lbl = lv_label_create(bar);
    lv_label_set_text(g_bt_lbl, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(g_bt_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_bt_lbl, COLOR_GRAY, 0);
    lv_obj_align(g_bt_lbl, LV_ALIGN_LEFT_MID, 36, 0);

    /* 右侧: 电池图标 */
    g_batt_lbl = lv_label_create(bar);
    lv_label_set_text(g_batt_lbl, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_font(g_batt_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_batt_lbl, COLOR_WHITE, 0);
    lv_obj_align(g_batt_lbl, LV_ALIGN_RIGHT_MID, -8, 0);

    /* 右侧: 时间显示 */
    g_time_lbl = lv_label_create(bar);
    lv_label_set_text(g_time_lbl, "00:00");
    lv_obj_set_style_text_font(g_time_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_time_lbl, COLOR_WHITE, 0);
    lv_obj_align(g_time_lbl, LV_ALIGN_RIGHT_MID, -34, 0);
}

/* 2. 表盘区域: 渐变背景 + 大号时间 + 日期星期 */
static void create_watch_face(lv_obj_t *p)
{
    lv_coord_t watch_h = SCREEN_H - STATUS_BAR_H;
    lv_obj_t *area = make_container(p, 0, STATUS_BAR_H, SCREEN_W, watch_h);

    /* 渐变背景: #1a1a2e → #0f3460 */
    lv_obj_set_style_bg_color(area, COLOR_WATCH_TOP, 0);
    lv_obj_set_style_bg_opa(area, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(area, COLOR_WATCH_BOT, 0);
    lv_obj_set_style_bg_grad_dir(area, LV_GRAD_DIR_VER, 0);

    /* 大号时间显示 (居中偏上, 使用 montserrat_44 字体) */
    g_clock_lbl = lv_label_create(area);
    lv_label_set_text(g_clock_lbl, "00:00");
    lv_obj_set_style_text_font(g_clock_lbl, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(g_clock_lbl, COLOR_WHITE, 0);
    lv_obj_align(g_clock_lbl, LV_ALIGN_CENTER, 0, -40);

    /* 日期星期 (位于时间下方) */
    g_date_lbl = lv_label_create(area);
    lv_label_set_text(g_date_lbl, "");
    lv_obj_set_style_text_font(g_date_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_date_lbl, COLOR_GRAY, 0);
    lv_obj_align(g_date_lbl, LV_ALIGN_CENTER, 0, 20);
}

/* 3. 控制中心 (下拉面板, 半透明遮罩 + 顶部面板) */
static void create_control_center(lv_obj_t *parent)
{
    /* ---- 半透明遮罩 (全屏, 点击关闭) ---- */
    g_cc_mask = lv_obj_create(parent);
    lv_obj_set_size(g_cc_mask, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_cc_mask, 0, 0);
    lv_obj_set_style_bg_color(g_cc_mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_cc_mask, LV_OPA_50, 0);
    lv_obj_set_style_border_width(g_cc_mask, 0, 0);
    lv_obj_set_style_radius(g_cc_mask, 0, 0);
    lv_obj_set_style_pad_all(g_cc_mask, 0, 0);
    lv_obj_clear_flag(g_cc_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_cc_mask, cb_cc_mask_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_cc_mask, cb_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(g_cc_mask, LV_OBJ_FLAG_HIDDEN);

    /* ---- 控制中心面板 (从顶部滑出) ---- */
    g_cc_panel = lv_obj_create(parent);
    lv_obj_set_size(g_cc_panel, SCREEN_W, CC_PANEL_H);
    lv_obj_set_pos(g_cc_panel, 0, -CC_PANEL_H);   /* 初始在屏幕上方隐藏 */
    lv_obj_set_style_bg_color(g_cc_panel, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(g_cc_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_cc_panel, 0, 0);
    lv_obj_set_style_border_width(g_cc_panel, 0, 0);
    lv_obj_set_style_pad_all(g_cc_panel, 12, 0);
    lv_obj_set_flex_flow(g_cc_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_cc_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_cc_panel, 10, 0);
    lv_obj_clear_flag(g_cc_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_cc_panel, cb_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(g_cc_panel, LV_OBJ_FLAG_HIDDEN);

    /* ---- 快捷开关网格 (2×2) ---- */
    lv_obj_t *grid = lv_obj_create(g_cc_panel);
    lv_obj_set_size(grid, SCREEN_W - 24, 180);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_radius(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    /* 4个快捷开关 */
    g_toggle_btns[TOGGLE_WIFI] = create_toggle(grid, LV_SYMBOL_WIFI,
                                               "WiFi", TOGGLE_WIFI);
    g_toggle_btns[TOGGLE_BT] = create_toggle(grid, LV_SYMBOL_BLUETOOTH,
                                             "蓝牙", TOGGLE_BT);
    g_toggle_btns[TOGGLE_MUTE] = create_toggle(grid, LV_SYMBOL_MUTE,
                                               "静音", TOGGLE_MUTE);
    g_toggle_btns[TOGGLE_SETTINGS] = create_toggle(grid, LV_SYMBOL_SETTINGS,
                                                   "设置", TOGGLE_SETTINGS);

    /* 初始化开关样式 */
    for (int i = 0; i < TOGGLE_COUNT; i++) {
        g_toggle_states[i] = false;
        update_toggle_style(i);
    }

    /* ---- 亮度滑块行 ---- */
    lv_obj_t *bright_row = lv_obj_create(g_cc_panel);
    lv_obj_set_size(bright_row, SCREEN_W - 24, 30);
    lv_obj_set_style_bg_opa(bright_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bright_row, 0, 0);
    lv_obj_set_style_pad_all(bright_row, 0, 0);
    lv_obj_set_style_radius(bright_row, 0, 0);
    lv_obj_set_flex_flow(bright_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bright_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bright_row, 8, 0);
    lv_obj_clear_flag(bright_row, LV_OBJ_FLAG_SCROLLABLE);

    /* 亮度图标 */
    lv_obj_t *bright_icon = lv_label_create(bright_row);
    lv_label_set_text(bright_icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_font(bright_icon, &esp_2, 0);
    lv_obj_set_style_text_color(bright_icon, COLOR_WHITE, 0);

    /* 亮度滑块 (BRIGHTNESS_MIN ~ BRIGHTNESS_MAX) */
    g_brightness_slider = lv_slider_create(bright_row);
    lv_obj_set_height(g_brightness_slider, 6);
    lv_obj_set_flex_grow(g_brightness_slider, 1);
    lv_slider_set_range(g_brightness_slider, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
    lv_slider_set_value(g_brightness_slider, bsp_brightness_get(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_brightness_slider, COLOR_TOGGLE_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_brightness_slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_brightness_slider, COLOR_WHITE, LV_PART_KNOB);
    lv_obj_add_event_cb(g_brightness_slider, cb_brightness,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(g_brightness_slider, cb_brightness,
                        LV_EVENT_RELEASED, NULL);

    /* ---- 音量滑块行 ---- */
    lv_obj_t *vol_row = lv_obj_create(g_cc_panel);
    lv_obj_set_size(vol_row, SCREEN_W - 24, 30);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_pad_all(vol_row, 0, 0);
    lv_obj_set_style_radius(vol_row, 0, 0);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vol_row, 8, 0);
    lv_obj_clear_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);

    /* 音量图标 */
    lv_obj_t *vol_icon = lv_label_create(vol_row);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(vol_icon, &esp_2, 0);
    lv_obj_set_style_text_color(vol_icon, COLOR_WHITE, 0);

    /* 音量滑块 (0~100) */
    g_volume_slider = lv_slider_create(vol_row);
    lv_obj_set_height(g_volume_slider, 6);
    lv_obj_set_flex_grow(g_volume_slider, 1);
    lv_slider_set_range(g_volume_slider, 0, 100);
    lv_slider_set_value(g_volume_slider, bsp_audio_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_volume_slider, COLOR_TOGGLE_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_volume_slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_volume_slider, COLOR_WHITE, LV_PART_KNOB);
    lv_obj_add_event_cb(g_volume_slider, cb_volume,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* ---- 上滑收起提示 ---- */
    lv_obj_t *hint = lv_label_create(g_cc_panel);
    lv_label_set_text(hint, "上滑收起");
    lv_obj_set_style_text_font(hint, &esp_2, 0);
    lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
}

/* 创建快捷开关 (圆角按钮 + 图标 + 文字标签) */
static lv_obj_t *create_toggle(lv_obj_t *parent, const char *icon,
                               const char *name, int id)
{
    /* 外层容器 (按钮 + 标签, 垂直排列) */
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, 75, 84);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 4, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    /* 圆角按钮 */
    lv_obj_t *btn = lv_btn_create(cont);
    lv_obj_set_size(btn, TOGGLE_BTN_SIZE, TOGGLE_BTN_SIZE);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb_toggle, LV_EVENT_CLICKED,
                        (void *)(intptr_t)id);

    /* 按钮内图标 */
    lv_obj_t *icon_lbl = lv_label_create(btn);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_set_style_text_font(icon_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(icon_lbl, COLOR_WHITE, 0);
    lv_obj_center(icon_lbl);
    g_toggle_icons[id] = icon_lbl;

    /* 文字标签 */
    lv_obj_t *name_lbl = lv_label_create(cont);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_font(name_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(name_lbl, COLOR_GRAY, 0);

    return btn;
}

/* ========================================================================
 * 事件回调
 * ======================================================================== */

/* 手势事件: 桌面下拉显示控制中心, 控制中心内部上滑收起 */
static void cb_gesture(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());

    if (obj == g_desktop_scr) {
        /* 桌面从顶部向下滑动显示控制中心 */
        if (dir == LV_DIR_BOTTOM) {
            ui_desktop_show_control_center();
        }
        /* 桌面从右向左滑动 (左滑) 进入应用中心 (控制中心打开时忽略) */
        if (dir == LV_DIR_LEFT && !g_cc_visible) {
            ui_apps_show();
        }
    } else {
        /* 控制中心面板/遮罩上检测 LV_DIR_BOTTOM 收起 */
        if (dir == LV_DIR_BOTTOM) {
            ui_desktop_hide_control_center();
        }
    }
}

/* 快捷开关点击 */
static void cb_toggle(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);

    /* 设置快捷: 直接跳转设置页面, 不切换开关状态 */
    if (id == TOGGLE_SETTINGS) {
        ui_settings_show();
        return;
    }

    /* 切换开关状态 */
    g_toggle_states[id] = !g_toggle_states[id];
    update_toggle_style(id);

    switch (id) {
        case TOGGLE_WIFI:
            /* WiFi 开关 - 仅更新视觉状态, 实际连接由外部管理 */
            break;
        case TOGGLE_BT:
            /* 蓝牙开关 - 仅更新视觉状态, 实际连接由外部管理 */
            break;
        case TOGGLE_MUTE:
            /* 静音开关 - 设置音量为0或恢复 */
            if (g_toggle_states[id]) {
                g_saved_volume = bsp_audio_get_volume();
                bsp_audio_set_volume(0);
                g_slider_updating = true;
                lv_slider_set_value(g_volume_slider, 0, LV_ANIM_ON);
                g_slider_updating = false;
            } else {
                bsp_audio_set_volume(g_saved_volume);
                g_slider_updating = true;
                lv_slider_set_value(g_volume_slider, g_saved_volume, LV_ANIM_ON);
                g_slider_updating = false;
            }
            break;
        default:
            break;
    }
}

/* 亮度滑块回调 */
static void cb_brightness(lv_event_t *e)
{
    if (g_slider_updating) return;
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    bsp_brightness_set((uint8_t)val);
    /* 松手时立即写入SD卡, 确保亮度持久保存 */
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        bsp_brightness_save();
    }
}

/* 音量滑块回调 */
static void cb_volume(lv_event_t *e)
{
    if (g_slider_updating) return;
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    bsp_audio_set_volume((uint8_t)val);

    /* 音量大于0时取消静音状态 */
    if (val > 0 && g_toggle_states[TOGGLE_MUTE]) {
        g_toggle_states[TOGGLE_MUTE] = false;
        update_toggle_style(TOGGLE_MUTE);
    }
}

/* 控制中心遮罩点击: 关闭控制中心 */
static void cb_cc_mask_click(lv_event_t *e)
{
    (void)e;
    ui_desktop_hide_control_center();
}

/* 控制中心滑动动画回调 */
static void cb_cc_anim(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

/* 控制中心隐藏动画完成回调 */
static void cb_cc_anim_done(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(g_cc_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_cc_panel, LV_OBJ_FLAG_HIDDEN);
}

/* 时间定时器: 每秒刷新时间 */
static void cb_timer(lv_timer_t *t)
{
    (void)t;
    update_time_display();
}

/* ========================================================================
 * 公开 API
 * ======================================================================== */

/* 初始化桌面 */
void ui_desktop_init(void)
{
    /* 创建桌面屏幕 */
    g_desktop_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_desktop_scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(g_desktop_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_desktop_scr, 0, 0);
    lv_obj_clear_flag(g_desktop_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 构建各区域 */
    create_status_bar(g_desktop_scr);
    create_watch_face(g_desktop_scr);
    create_control_center(g_desktop_scr);

    /* 桌面手势: 顶部下拉显示控制中心 */
    lv_obj_add_event_cb(g_desktop_scr, cb_gesture, LV_EVENT_GESTURE, NULL);

    /* 时间刷新定时器 (每 1000ms) */
    lv_timer_create(cb_timer, 1000, NULL);

    /* 初始时间更新 */
    update_time_display();

    /* 加载桌面屏幕 */
    lv_screen_load(g_desktop_scr);
}

/* 显示桌面 */
void ui_desktop_show(void)
{
    /* 确保控制中心已收起 */
    g_cc_visible = false;
    lv_obj_add_flag(g_cc_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_cc_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(g_cc_panel, 0, -CC_PANEL_H);

    lv_screen_load(g_desktop_scr);
}

/* 更新 WiFi 状态 */
void ui_desktop_update_wifi(bool connected)
{
    g_wifi_connected = connected;
    if (connected) {
        lv_obj_set_style_text_color(g_wifi_lbl, COLOR_WHITE, 0);
    } else {
        lv_obj_set_style_text_color(g_wifi_lbl, COLOR_GRAY, 0);
    }
    /* 同步控制中心开关 */
    g_toggle_states[TOGGLE_WIFI] = connected;
    update_toggle_style(TOGGLE_WIFI);
}

/* 更新蓝牙状态 */
void ui_desktop_update_bt(bool connected)
{
    g_bt_connected = connected;
    if (connected) {
        lv_obj_set_style_text_color(g_bt_lbl, COLOR_WHITE, 0);
    } else {
        lv_obj_set_style_text_color(g_bt_lbl, COLOR_GRAY, 0);
    }
    /* 同步控制中心开关 */
    g_toggle_states[TOGGLE_BT] = connected;
    update_toggle_style(TOGGLE_BT);
}

/* 刷新时间显示 */
void ui_desktop_update_time(void)
{
    update_time_display();
}

/* 显示下拉控制中心 */
void ui_desktop_show_control_center(void)
{
    if (g_cc_visible) return;
    g_cc_visible = true;

    /* 显示遮罩和面板 */
    lv_obj_clear_flag(g_cc_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_cc_panel, LV_OBJ_FLAG_HIDDEN);

    /* 面板从顶部滑入动画 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_cc_panel);
    lv_anim_set_values(&a, -CC_PANEL_H, 0);
    lv_anim_set_time(&a, 300);
    lv_anim_set_exec_cb(&a, cb_cc_anim);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* 隐藏下拉控制中心 */
void ui_desktop_hide_control_center(void)
{
    if (!g_cc_visible) return;
    g_cc_visible = false;

    /* 面板滑出顶部动画 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_cc_panel);
    lv_anim_set_values(&a, 0, -CC_PANEL_H);
    lv_anim_set_time(&a, 300);
    lv_anim_set_exec_cb(&a, cb_cc_anim);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, cb_cc_anim_done);
    lv_anim_start(&a);
}
