/*******************************************************************************
 * 设置页面 UI 模块实现 - LVGL 9.x
 *
 * 屏幕: 262×928 (竖屏) RGB565
 *
 * 结构:
 *   主设置列表页 → 蓝牙子页面 / WiFi子页面 / 亮度子页面
 *                  / 音量子页面 / 关于子页面 / 硬件测试 / 主题颜色
 *
 * 导航: 前进 LV_SCR_LOAD_ANIM_MOVE_LEFT, 返回 LV_SCR_LOAD_ANIM_MOVE_RIGHT
 * 蓝牙和WiFi子页面已迁移到独立模块 ui_bluetooth.c / ui_wifi.c
 ******************************************************************************/

#include "ui_settings.h"
#include "ui_lvgl_async.h"
#include "bsp_hw_test.h"
#include "bsp_theme.h"
#include "bsp_memory.h"
#include "bsp_audio.h"
#include "audio_player.h"
#include "ui_gesture.h"
#include "ui_navbar.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if (esp_idf_version >= ESP_IDF_VERSION_VAL(5, 2, 0))
#include "freertos/idf_additions.h"
#endif
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* 字体 (在 esp_2.41ttf.c 中定义) */
extern const lv_font_t esp_2;

/* 返回桌面函数 (由桌面模块提供) */
extern void ui_desktop_show(void);

/* ======================== 颜色定义 (从主题模块获取) ======================== */
static inline lv_color_t theme_bg(void)     { return bsp_theme_colors()->bg; }
static inline lv_color_t theme_card(void)   { return bsp_theme_colors()->card; }
static inline lv_color_t theme_primary(void){ return bsp_theme_colors()->primary; }
static inline lv_color_t theme_accent(void) { return bsp_theme_colors()->accent; }

#define COLOR_BG        theme_bg()
#define COLOR_CARD      theme_card()
#define COLOR_PRIMARY   theme_primary()
#define COLOR_ACCENT    theme_accent()
#define COLOR_WHITE     lv_color_hex(0xffffff)
#define COLOR_GRAY      lv_color_hex(0x888888)

/* ======================== 屏幕尺寸 / 布局 ======================== */
#define SCREEN_W       262
#define SCREEN_H       928
#define NAVBAR_H_SET   52                       /* 底部导航栏高度 (为布局预留空间) */
#define TITLE_BAR_H    40
#define LIST_ITEM_H    50
#define CONTENT_PAD    6
#define CONTENT_W      (SCREEN_W - 2 * CONTENT_PAD)

/* ======================== 静态 screen 对象 ======================== */
static lv_obj_t *g_scr_main       = NULL;
static lv_obj_t *g_scr_brightness = NULL;
static lv_obj_t *g_scr_volume     = NULL;
static lv_obj_t *g_scr_about      = NULL;
static lv_obj_t *g_scr_hw_test    = NULL;
static lv_obj_t *g_scr_theme      = NULL;
static lv_obj_t *g_scr_bg_mgr     = NULL;

/* ======================== 静态 UI 对象引用 ======================== */
/* 亮度页面 */
static lv_obj_t *g_bright_slider;
static lv_obj_t *g_bright_value_lbl;
static lv_obj_t *g_bright_preview;

/* 音量页面 */
static lv_obj_t *g_vol_slider;
static lv_obj_t *g_vol_value_lbl;
static lv_obj_t *g_vol_icon;
static bool      g_muted = false;

/* 防止滑块回调递归 */
static bool g_slider_updating = false;

/* 页面加载保护 - 防止动画冲突导致卡死 */
static volatile bool g_page_loading = false;
static uint32_t g_last_load_time = 0;
#define PAGE_LOAD_DEBOUNCE_MS 500  /* 页面切换防抖延迟 */

/* 主题切换: 延迟重建标志, 由 lv_timer 在 LVGL 线程处理 */
static lv_timer_t *s_theme_rebuild_timer = NULL;

/* ======================== 前置声明 ======================== */
static void create_settings_main(void);
static void create_brightness_page(void);
static void create_volume_page(void);
static void create_about_page(void);
static void create_hw_test_page(void);
static void create_theme_page(void);
static void create_bg_mgr_page(void);

/* 导航回调 */
static void cb_back_to_desktop(lv_event_t *e);
static void cb_back_to_main(lv_event_t *e);
static void swipe_back_to_main(void);
static void swipe_back_to_desktop(void);
static void cb_open_bt(lv_event_t *e);
static void cb_open_wifi(lv_event_t *e);
static void cb_open_brightness(lv_event_t *e);
static void cb_open_volume(lv_event_t *e);
static void cb_open_about(lv_event_t *e);
static void cb_open_hw_test(lv_event_t *e);
static void cb_run_hw_test(lv_event_t *e);
static void cb_open_theme(lv_event_t *e);
static void cb_theme_select(lv_event_t *e);
static void cb_open_bg_mgr(lv_event_t *e);
static void bg_mgr_stop_timer(void);

/* 交互回调 */
static void cb_brightness_slider(lv_event_t *e);
static void cb_volume_slider(lv_event_t *e);
static void cb_mute(lv_event_t *e);

/* 辅助函数 */
static lv_obj_t *create_screen(void);
static lv_obj_t *make_container(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                lv_coord_t w, lv_coord_t h);
static void create_title_bar(lv_obj_t *parent, const char *title);
static lv_obj_t *create_content_area(lv_obj_t *parent);
static lv_obj_t *create_list_item(lv_obj_t *parent, const char *icon,
                                  const char *text, lv_event_cb_t cb);
static lv_obj_t *create_info_row(lv_obj_t *parent, const char *key,
                                 const char *value);
static lv_obj_t *create_button(lv_obj_t *parent, const char *text,
                               lv_event_cb_t cb);
static void create_about_info(lv_obj_t *parent, const char *key,
                              const char *value);
static void update_brightness_preview(uint8_t brightness);
static void update_volume_display(uint8_t volume);

static void safe_screen_load(lv_obj_t *target, bool anim);

/* 延迟清除页面加载标志的定时器回调 */
static void clear_loading_timer_cb(lv_timer_t *t)
{
    g_page_loading = false;
    lv_timer_del(t);
}

/* ========================================================================
 * 页面加载辅助函数
 * ======================================================================== */

static void safe_screen_load(lv_obj_t *target, bool anim)
{
    uint32_t now = lv_tick_get();

    if (g_page_loading && (now - g_last_load_time) < PAGE_LOAD_DEBOUNCE_MS) {
        ESP_LOGW("ui_settings", "页面切换过于频繁, 防抖保护");
        return;
    }

    g_page_loading = true;
    g_last_load_time = now;

    if (anim) {
        lv_screen_load_anim(target, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    } else {
        lv_screen_load(target);
    }

    lv_timer_t *timer = lv_timer_create(clear_loading_timer_cb, 350, NULL);
    lv_timer_set_repeat_count(timer, 1);
}

/* ========================================================================
 * 辅助函数
 * ======================================================================== */

static lv_obj_t *create_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    return scr;
}

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

static void create_title_bar(lv_obj_t *parent, const char *title)
{
    lv_obj_t *bar = make_container(parent, 0, 0, SCREEN_W, TITLE_BAR_H);
    lv_obj_set_style_bg_color(bar, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    /* 左侧手势提示 */
    lv_obj_t *hint = lv_label_create(bar);
    lv_label_set_text(hint, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(hint, &esp_2, 0);
    lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_50, 0);
    lv_obj_align(hint, LV_ALIGN_LEFT_MID, 8, 0);

    /* 标题 */
    lv_obj_t *title_lbl = lv_label_create(bar);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(title_lbl, COLOR_WHITE, 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, 0);
}

static lv_obj_t *create_content_area(lv_obj_t *parent)
{
    lv_obj_t *content = lv_obj_create(parent);
    lv_obj_set_pos(content, 0, TITLE_BAR_H);
    lv_obj_set_size(content, SCREEN_W, SCREEN_H - TITLE_BAR_H - NAVBAR_H_SET);
    lv_obj_set_style_bg_color(content, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_left(content, CONTENT_PAD, 0);
    lv_obj_set_style_pad_right(content, CONTENT_PAD, 0);
    lv_obj_set_style_pad_top(content, CONTENT_PAD, 0);
    lv_obj_set_style_pad_bottom(content, CONTENT_PAD, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, 6, 0);
    return content;
}

static lv_obj_t *create_list_item(lv_obj_t *parent, const char *icon,
                                  const char *text, lv_event_cb_t cb)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_height(item, LIST_ITEM_H);
    lv_obj_set_style_bg_color(item, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(item, 6, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_bg_color(item, COLOR_PRIMARY, LV_STATE_PRESSED);

    lv_obj_t *icon_lbl = lv_label_create(item);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_set_style_text_font(icon_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(icon_lbl, COLOR_ACCENT, 0);
    lv_obj_align(icon_lbl, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *text_lbl = lv_label_create(item);
    lv_label_set_text(text_lbl, text);
    lv_obj_set_style_text_font(text_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(text_lbl, COLOR_WHITE, 0);
    lv_obj_align(text_lbl, LV_ALIGN_LEFT_MID, 42, 0);

    lv_obj_t *arrow = lv_label_create(item);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, &esp_2, 0);
    lv_obj_set_style_text_color(arrow, COLOR_GRAY, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(item, cb, LV_EVENT_CLICKED, NULL);
    return item;
}

static lv_obj_t *create_info_row(lv_obj_t *parent, const char *key,
                                 const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 32);
    lv_obj_set_style_bg_color(row, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_left(row, 10, 0);
    lv_obj_set_style_pad_right(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *key_lbl = lv_label_create(row);
    lv_label_set_text(key_lbl, key);
    lv_obj_set_style_text_font(key_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(key_lbl, COLOR_GRAY, 0);
    lv_obj_align(key_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *val_lbl = lv_label_create(row);
    lv_label_set_text(val_lbl, value);
    lv_obj_set_style_text_font(val_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(val_lbl, COLOR_WHITE, 0);
    lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    return val_lbl;
}

static lv_obj_t *create_button(lv_obj_t *parent, const char *text,
                               lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, 40);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 4, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &esp_2, 0);
    lv_obj_set_style_text_color(lbl, COLOR_WHITE, 0);
    lv_obj_center(lbl);

    return btn;
}

static void create_about_info(lv_obj_t *parent, const char *key,
                              const char *value)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s: %s", key, value);

    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &esp_2, 0);
    lv_obj_set_style_text_color(lbl, COLOR_WHITE, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(lbl, CONTENT_W);
    lv_obj_set_style_pad_left(lbl, 4, 0);
    lv_obj_set_style_pad_bottom(lbl, 4, 0);
}

static void update_brightness_preview(uint8_t brightness)
{
    if (g_bright_preview == NULL) return;
    lv_obj_set_style_bg_color(g_bright_preview,
                              lv_color_make(brightness, brightness, brightness), 0);
    lv_obj_set_style_bg_opa(g_bright_preview, LV_OPA_COVER, 0);
}

static void update_volume_display(uint8_t volume)
{
    if (g_vol_value_lbl == NULL) return;

    char buf[32];
    snprintf(buf, sizeof(buf), "音量: %d%%", volume);
    lv_label_set_text(g_vol_value_lbl, buf);

    if (g_vol_icon != NULL) {
        if (g_muted || volume == 0) {
            lv_label_set_text(g_vol_icon, LV_SYMBOL_MUTE);
            lv_obj_set_style_text_color(g_vol_icon, COLOR_GRAY, 0);
        } else {
            lv_label_set_text(g_vol_icon, LV_SYMBOL_VOLUME_MAX);
            lv_obj_set_style_text_color(g_vol_icon, COLOR_WHITE, 0);
        }
    }
}

/* ========================================================================
 * 重建所有设置页 (主题切换后调用)
 * ======================================================================== */

static void destroy_settings_pages(void)
{
    /* 后台管理页: 先停 1s 刷新定时器再删页面, 防止悬空访问 */
    bg_mgr_stop_timer();
    if (g_scr_bg_mgr)     { lv_obj_del(g_scr_bg_mgr);     g_scr_bg_mgr = NULL; }
    if (g_scr_theme)      { lv_obj_del(g_scr_theme);      g_scr_theme = NULL; }
    if (g_scr_hw_test)    { lv_obj_del(g_scr_hw_test);    g_scr_hw_test = NULL; }
    if (g_scr_about)      { lv_obj_del(g_scr_about);      g_scr_about = NULL; }
    if (g_scr_volume)     { lv_obj_del(g_scr_volume);     g_scr_volume = NULL; }
    if (g_scr_brightness) { lv_obj_del(g_scr_brightness); g_scr_brightness = NULL; }
    /* g_scr_main 不删, 重建主页面 */
    if (g_scr_main)       { lv_obj_del(g_scr_main);       g_scr_main = NULL; }

    /* 通知 ui_wifi/ui_bluetooth 重建以应用新主题 */
    extern void ui_wifi_invalidate(void);
    extern void ui_bluetooth_invalidate(void);
    ui_wifi_invalidate();
    ui_bluetooth_invalidate();
}

static void rebuild_after_theme_cb(lv_timer_t *t)
{
    (void)t;
    s_theme_rebuild_timer = NULL;

    lv_obj_t *active = lv_screen_active();
    destroy_settings_pages();

    /* 先建主页 */
    create_settings_main();
    /* 切回主页, 触发LVGL重绘颜色 */
    lv_screen_load(g_scr_main);

    ESP_LOGI("ui_settings", "主题切换完成, 当前: %s",
             bsp_theme_name(bsp_theme_get()));
}

/* ========================================================================
 * 页面创建
 * ======================================================================== */

/* 1. 主设置列表页 */
static void create_settings_main(void)
{
    g_scr_main = create_screen();
    create_title_bar(g_scr_main, "设置");
    ui_edge_swipe_add(g_scr_main, swipe_back_to_desktop);

    lv_obj_t *content = create_content_area(g_scr_main);

    create_list_item(content, LV_SYMBOL_BLUETOOTH,  "蓝牙耳机", cb_open_bt);
    create_list_item(content, LV_SYMBOL_WIFI,       "WiFi连接", cb_open_wifi);
    create_list_item(content, LV_SYMBOL_IMAGE,      "亮度调节", cb_open_brightness);
    create_list_item(content, LV_SYMBOL_VOLUME_MAX, "音量调节", cb_open_volume);
    create_list_item(content, LV_SYMBOL_LIST,       "硬件测试", cb_open_hw_test);
    create_list_item(content, LV_SYMBOL_EYE_OPEN,   "后台管理", cb_open_bg_mgr);
    create_list_item(content, LV_SYMBOL_REFRESH,    "主题颜色", cb_open_theme);
    create_list_item(content, LV_SYMBOL_SETTINGS,   "关于设备", cb_open_about);
}

/* 2. 亮度子页面 */
static void create_brightness_page(void)
{
    g_scr_brightness = create_screen();
    create_title_bar(g_scr_brightness, "亮度调节");
    ui_edge_swipe_add(g_scr_brightness, swipe_back_to_main);

    g_bright_value_lbl = lv_label_create(g_scr_brightness);
    lv_obj_set_style_text_font(g_bright_value_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_bright_value_lbl, COLOR_WHITE, 0);
    lv_obj_align(g_bright_value_lbl, LV_ALIGN_TOP_MID, 0, TITLE_BAR_H + 16);

    /* 水平滑块 (竖屏宽度有限, 水平更易操作) */
    g_bright_slider = lv_slider_create(g_scr_brightness);
    lv_obj_set_size(g_bright_slider, SCREEN_W - 24, 24);
    lv_obj_align(g_bright_slider, LV_ALIGN_TOP_MID, 0, TITLE_BAR_H + 60);
    lv_slider_set_range(g_bright_slider, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
    lv_slider_set_value(g_bright_slider, bsp_brightness_get(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_bright_slider, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_bright_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bright_slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_bright_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_bright_slider, COLOR_WHITE, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_bright_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(g_bright_slider, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(g_bright_slider, 12, LV_PART_INDICATOR);
    lv_obj_add_event_cb(g_bright_slider, cb_brightness_slider,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(g_bright_slider, cb_brightness_slider,
                        LV_EVENT_RELEASED, NULL);

    /* 预览矩形 */
    lv_obj_t *preview_lbl = lv_label_create(g_scr_brightness);
    lv_label_set_text(preview_lbl, "预览");
    lv_obj_set_style_text_font(preview_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(preview_lbl, COLOR_GRAY, 0);
    lv_obj_align(preview_lbl, LV_ALIGN_TOP_MID, 0, TITLE_BAR_H + 110);

    g_bright_preview = lv_obj_create(g_scr_brightness);
    lv_obj_set_size(g_bright_preview, SCREEN_W - 24, 60);
    lv_obj_set_style_radius(g_bright_preview, 6, 0);
    lv_obj_set_style_border_width(g_bright_preview, 1, 0);
    lv_obj_set_style_border_color(g_bright_preview, COLOR_GRAY, 0);
    lv_obj_set_style_pad_all(g_bright_preview, 0, 0);
    lv_obj_clear_flag(g_bright_preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(g_bright_preview, LV_ALIGN_TOP_MID, 0, TITLE_BAR_H + 138);

    /* 提示 */
    lv_obj_t *hint = lv_label_create(g_scr_brightness);
    lv_label_set_text(hint, "滑动调节亮度, 自动保存");
    lv_obj_set_style_text_font(hint, &esp_2, 0);
    lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, TITLE_BAR_H + 210);

    char buf[32];
    snprintf(buf, sizeof(buf), "亮度: %d", bsp_brightness_get());
    lv_label_set_text(g_bright_value_lbl, buf);
    update_brightness_preview(bsp_brightness_get());
}

/* 3. 音量子页面 */
static void create_volume_page(void)
{
    g_scr_volume = create_screen();
    create_title_bar(g_scr_volume, "音量调节");
    ui_edge_swipe_add(g_scr_volume, swipe_back_to_main);

    /* 音量图标 */
    lv_obj_t *icon_cont = lv_obj_create(g_scr_volume);
    lv_obj_set_size(icon_cont, 56, 56);
    lv_obj_set_style_radius(icon_cont, 28, 0);
    lv_obj_set_style_bg_color(icon_cont, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(icon_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon_cont, 2, 0);
    lv_obj_set_style_border_color(icon_cont, COLOR_ACCENT, 0);
    lv_obj_set_style_pad_all(icon_cont, 0, 0);
    lv_obj_clear_flag(icon_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(icon_cont, LV_ALIGN_TOP_MID, 0, TITLE_BAR_H + 16);

    g_vol_icon = lv_label_create(icon_cont);
    lv_label_set_text(g_vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(g_vol_icon, &esp_2, 0);
    lv_obj_set_style_text_color(g_vol_icon, COLOR_WHITE, 0);
    lv_obj_center(g_vol_icon);

    g_vol_value_lbl = lv_label_create(g_scr_volume);
    lv_obj_set_style_text_font(g_vol_value_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_vol_value_lbl, COLOR_WHITE, 0);
    lv_obj_align(g_vol_value_lbl, LV_ALIGN_TOP_MID, 0, TITLE_BAR_H + 88);

    /* 水平滑块 */
    g_vol_slider = lv_slider_create(g_scr_volume);
    lv_obj_set_size(g_vol_slider, SCREEN_W - 24, 24);
    lv_obj_align(g_vol_slider, LV_ALIGN_TOP_MID, 0, TITLE_BAR_H + 130);
    lv_slider_set_range(g_vol_slider, 0, 100);
    lv_slider_set_value(g_vol_slider, bsp_audio_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_vol_slider, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_vol_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_vol_slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_vol_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_vol_slider, COLOR_WHITE, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_vol_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(g_vol_slider, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(g_vol_slider, 12, LV_PART_INDICATOR);
    lv_obj_add_event_cb(g_vol_slider, cb_volume_slider,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* 静音按钮 */
    lv_obj_t *mute_btn = lv_btn_create(g_scr_volume);
    lv_obj_set_size(mute_btn, 110, 36);
    lv_obj_set_style_radius(mute_btn, 6, 0);
    lv_obj_set_style_bg_color(mute_btn, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(mute_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(mute_btn, 0, 0);
    lv_obj_set_style_border_width(mute_btn, 1, 0);
    lv_obj_set_style_border_color(mute_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_pad_all(mute_btn, 0, 0);
    lv_obj_align(mute_btn, LV_ALIGN_TOP_MID, 0, TITLE_BAR_H + 180);
    lv_obj_add_event_cb(mute_btn, cb_mute, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mute_lbl = lv_label_create(mute_btn);
    lv_label_set_text(mute_lbl, LV_SYMBOL_MUTE " 静音");
    lv_obj_set_style_text_font(mute_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(mute_lbl, COLOR_WHITE, 0);
    lv_obj_center(mute_lbl);

    /* 提示 */
    lv_obj_t *hint = lv_label_create(g_scr_volume);
    lv_label_set_text(hint, "滑动调节音量, 自动保存");
    lv_obj_set_style_text_font(hint, &esp_2, 0);
    lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, TITLE_BAR_H + 230);

    update_volume_display(bsp_audio_get_volume());
}

/* 4. 关于子页面 */
static void create_about_page(void)
{
    g_scr_about = create_screen();
    create_title_bar(g_scr_about, "关于设备");
    ui_edge_swipe_add(g_scr_about, swipe_back_to_main);

    lv_obj_t *content = create_content_area(g_scr_about);

    /* 设备图标 */
    lv_obj_t *icon_cont = lv_obj_create(content);
    lv_obj_set_size(icon_cont, 64, 64);
    lv_obj_set_style_radius(icon_cont, 32, 0);
    lv_obj_set_style_bg_color(icon_cont, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(icon_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon_cont, 2, 0);
    lv_obj_set_style_border_color(icon_cont, COLOR_ACCENT, 0);
    lv_obj_set_style_pad_all(icon_cont, 0, 0);
    lv_obj_clear_flag(icon_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(icon_cont);
    lv_label_set_text(icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(icon, &esp_2, 0);
    lv_obj_set_style_text_color(icon, COLOR_WHITE, 0);
    lv_obj_center(icon);

    /* 间隔 */
    lv_obj_t *spacer = lv_obj_create(content);
    lv_obj_set_height(spacer, 8);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);

    create_about_info(content, "设备名称", "ESP32-P4 播放器");
    create_about_info(content, "芯片",     "ESP32-P4");
    create_about_info(content, "屏幕",     "262x928 AMOLED");
    create_about_info(content, "音频DAC",  "CS43131");
    create_about_info(content, "触摸",     "CST3530");
    create_about_info(content, "固件版本", "v1.0.0");

    char build_buf[40];
    snprintf(build_buf, sizeof(build_buf), "%s %s", __DATE__, __TIME__);
    create_about_info(content, "构建日期", build_buf);

    const char *sd_status = bsp_sd_card_is_mounted() ? "已挂载" : "未挂载";
    create_about_info(content, "SD卡", sd_status);

    char music_buf[20];
    snprintf(music_buf, sizeof(music_buf), "%d 首",
             audio_player_get_music_count());
    create_about_info(content, "音乐数量", music_buf);
}

/* ========================================================================
 * 事件回调 — 导航
 * ======================================================================== */

static void cb_back_to_desktop(lv_event_t *e)
{
    (void)e;
    /* 通过导航栏返回键回到桌面 */
    ui_navbar_home();
}

static void cb_back_to_main(lv_event_t *e)
{
    (void)e;
    if (g_scr_main != NULL) {
        /* 返回设置主页 (使用导航栏替换栈顶, 避免栈增长) */
        ui_navbar_replace_top(g_scr_main, false);
    }
}

static void swipe_back_to_main(void)
{
    if (g_scr_main != NULL) {
        ui_navbar_replace_top(g_scr_main, false);
    }
}

static void swipe_back_to_desktop(void)
{
    ui_navbar_home();
}

/* 进入蓝牙子页面 */
static void cb_open_bt(lv_event_t *e)
{
    (void)e;
    extern lv_obj_t *ui_bluetooth_create_page(void);
    lv_obj_t *scr = ui_bluetooth_create_page();
    lv_screen_load(scr);
}

/* 进入 WiFi 子页面 */
static void cb_open_wifi(lv_event_t *e)
{
    (void)e;
    extern lv_obj_t *ui_wifi_create_page(void);
    lv_obj_t *scr = ui_wifi_create_page();
    lv_screen_load(scr);
}

static void cb_open_brightness(lv_event_t *e)
{
    (void)e;
    if (g_scr_brightness == NULL) {
        create_brightness_page();
    }
    safe_screen_load(g_scr_brightness, true);
}

static void cb_open_volume(lv_event_t *e)
{
    (void)e;
    if (g_scr_volume == NULL) {
        create_volume_page();
    }
    safe_screen_load(g_scr_volume, true);
}

static void cb_open_about(lv_event_t *e)
{
    (void)e;
    if (g_scr_about == NULL) {
        create_about_page();
    }
    safe_screen_load(g_scr_about, true);
}

static void cb_open_hw_test(lv_event_t *e)
{
    (void)e;
    if (g_scr_hw_test == NULL) {
        create_hw_test_page();
    }
    safe_screen_load(g_scr_hw_test, true);
}

/* ========================================================================
 * 事件回调 — 交互
 * ======================================================================== */

static void cb_brightness_slider(lv_event_t *e)
{
    if (g_slider_updating) return;
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);

    bsp_brightness_set((uint8_t)val);
    /* 松手时立即写入SD卡, 确保亮度持久保存 */
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        bsp_brightness_save();
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "亮度: %d", (int)val);
    lv_label_set_text(g_bright_value_lbl, buf);

    update_brightness_preview((uint8_t)val);
}

static void cb_volume_slider(lv_event_t *e)
{
    if (g_slider_updating) return;
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);

    g_muted = false;
    /* 双路驱动: BT 路径软件增益 + I2S 路径 CS43131 硬件音量 (含NVS保存).
     * 旧版只调 DAC, 蓝牙播放时拖音量条实际音量纹丝不动 */
    audio_player_set_volume((uint8_t)val);
    update_volume_display((uint8_t)val);
}

static void cb_mute(lv_event_t *e)
{
    (void)e;
    g_muted = !g_muted;
    if (g_muted) {
        audio_player_set_volume(0);
    } else {
        uint8_t vol = (uint8_t)lv_slider_get_value(g_vol_slider);
        audio_player_set_volume(vol);
    }
    update_volume_display((uint8_t)lv_slider_get_value(g_vol_slider));
}

/* 5. 硬件测试子页面 */
static lv_obj_t *g_hw_test_btn;
static lv_obj_t *g_hw_test_list;

static void create_hw_test_page(void)
{
    g_scr_hw_test = create_screen();
    create_title_bar(g_scr_hw_test, "硬件测试");
    ui_edge_swipe_add(g_scr_hw_test, swipe_back_to_main);

    lv_obj_t *content = create_content_area(g_scr_hw_test);

    g_hw_test_btn = create_button(content, "开始测试", cb_run_hw_test);

    g_hw_test_list = lv_list_create(content);
    lv_obj_set_width(g_hw_test_list, LV_PCT(100));
    lv_obj_set_height(g_hw_test_list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g_hw_test_list, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(g_hw_test_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_hw_test_list, 0, 0);
    lv_obj_set_style_radius(g_hw_test_list, 6, 0);
    lv_obj_set_style_pad_all(g_hw_test_list, 4, 0);
    lv_obj_set_style_text_font(g_hw_test_list, &esp_2, 0);

    lv_obj_t *hint = lv_list_add_text(g_hw_test_list, "点击上方按钮开始测试");
    lv_obj_set_style_text_font(hint, &esp_2, 0);
    lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
}

static hw_test_result_t s_hw_results[HW_TEST_COUNT];
static volatile bool s_hw_testing = false;
static lv_timer_t *s_hw_poll_timer = NULL;

static void hw_test_task(void *arg)
{
    s_hw_testing = true;
    memset(s_hw_results, 0, sizeof(s_hw_results));
    bsp_hw_test_run_all(s_hw_results);
    s_hw_testing = false;
    vTaskDelete(NULL);
}

static void hw_test_poll_timer_cb(lv_timer_t *tmr)
{
    (void)tmr;
    if (s_hw_testing) return;

    if (s_hw_poll_timer) {
        lv_timer_del(s_hw_poll_timer);
        s_hw_poll_timer = NULL;
    }

    if (g_hw_test_list == NULL) return;

    lv_obj_clean(g_hw_test_list);

    for (int i = 0; i < HW_TEST_COUNT; i++) {
        char buf[128];
        const char *status_icon = s_hw_results[i].passed ?
                                  LV_SYMBOL_OK : LV_SYMBOL_WARNING;
        const char *status_text = s_hw_results[i].passed ? "通过" : "失败";

        snprintf(buf, sizeof(buf), "%s  %s: %s",
                 status_icon, bsp_hw_test_name((hw_test_id_t)i), status_text);

        lv_obj_t *item = lv_list_add_btn(g_hw_test_list, bsp_hw_test_icon((hw_test_id_t)i), buf);
        lv_obj_set_style_text_font(item, &esp_2, 0);
        lv_obj_set_style_text_color(item,
                                    s_hw_results[i].passed ? COLOR_WHITE : COLOR_ACCENT, 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *detail = lv_list_add_text(g_hw_test_list, s_hw_results[i].detail);
        lv_obj_set_style_text_font(detail, &esp_2, 0);
        lv_obj_set_style_text_color(detail, COLOR_GRAY, 0);
    }

    if (g_hw_test_btn) {
        lv_obj_t *lbl = lv_obj_get_child(g_hw_test_btn, 0);
        if (lbl) {
            lv_label_set_text(lbl, "重新测试");
        }
    }
}

static void cb_run_hw_test(lv_event_t *e)
{
    (void)e;
    if (s_hw_testing) return;

    if (g_hw_test_btn) {
        lv_obj_t *lbl = lv_obj_get_child(g_hw_test_btn, 0);
        if (lbl) {
            lv_label_set_text(lbl, "测试中...");
        }
    }

    if (g_hw_test_list) {
        lv_obj_clean(g_hw_test_list);
        lv_obj_t *hint = lv_list_add_text(g_hw_test_list, "正在测试, 请稍候...");
        lv_obj_set_style_text_font(hint, &esp_2, 0);
        lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
    }

    if (s_hw_poll_timer) {
        lv_timer_del(s_hw_poll_timer);
        s_hw_poll_timer = NULL;
    }

    /* 测试任务栈优先放 PSRAM (IDF 的栈参数单位为字节), 失败回退内部 RAM */
    BaseType_t ok = pdFAIL;
#if (esp_idf_version >= ESP_IDF_VERSION_VAL(5, 2, 0))
    ok = xTaskCreateWithCaps(hw_test_task, "hw_test", 6144,
                             NULL, 2, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGW("ui_settings", "hw_test PSRAM 栈分配失败, 回退内部 RAM");
    }
#endif
    if (ok != pdPASS) {
        xTaskCreate(hw_test_task, "hw_test", 6144, NULL, 2, NULL);
    }
    s_hw_poll_timer = lv_timer_create(hw_test_poll_timer_cb, 200, NULL);
}

/* 6. 主题颜色选择子页面 */
static void create_theme_page(void)
{
    g_scr_theme = create_screen();
    create_title_bar(g_scr_theme, "主题颜色");
    ui_edge_swipe_add(g_scr_theme, swipe_back_to_main);

    lv_obj_t *content = create_content_area(g_scr_theme);

    lv_obj_t *hint = lv_label_create(content);
    lv_label_set_text(hint, "点击切换主题");
    lv_obj_set_style_text_font(hint, &esp_2, 0);
    lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(hint, 8, 0);

    for (int i = 0; i < THEME_COUNT; i++) {
        lv_obj_t *item = lv_obj_create(content);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, 44);
        lv_obj_set_style_bg_color(item, COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(item, 6, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_left(item, 10, 0);
        lv_obj_set_style_pad_right(item, 10, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);

        theme_id_t current = bsp_theme_get();
        if (i == (int)current) {
            lv_obj_set_style_border_color(item, theme_accent(), 0);
            lv_obj_set_style_border_width(item, 2, 0);
        }

        lv_obj_set_user_data(item, (void *)(intptr_t)i);

        /* 色块 */
        lv_obj_t *color_box = lv_obj_create(item);
        lv_obj_set_size(color_box, 24, 24);
        lv_obj_set_style_bg_color(color_box, bsp_theme_preview_color((theme_id_t)i), 0);
        lv_obj_set_style_bg_opa(color_box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(color_box, 12, 0);
        lv_obj_set_style_border_width(color_box, 0, 0);
        lv_obj_clear_flag(color_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(color_box, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *name = lv_label_create(item);
        lv_label_set_text(name, bsp_theme_name((theme_id_t)i));
        lv_obj_set_style_text_font(name, &esp_2, 0);
        lv_obj_set_style_text_color(name, COLOR_WHITE, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 34, 0);

        if (i == (int)current) {
            lv_obj_t *check = lv_label_create(item);
            lv_label_set_text(check, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(check, theme_accent(), 0);
            lv_obj_set_style_text_font(check, &esp_2, 0);
            lv_obj_align(check, LV_ALIGN_RIGHT_MID, 0, 0);
        }

        lv_obj_add_event_cb(item, cb_theme_select, LV_EVENT_CLICKED, NULL);
    }
}

/* 主题切换回调 - 调度重建 (避免在回调中直接操作UI对象导致卡死) */
static void cb_theme_select(lv_event_t *e)
{
    lv_obj_t *item = lv_event_get_target(e);
    intptr_t id = (intptr_t)lv_obj_get_user_data(item);

    if (id >= 0 && id < THEME_COUNT) {
        theme_id_t new_theme = (theme_id_t)id;
        if (new_theme == bsp_theme_get()) return;

        bsp_theme_set(new_theme);
        ESP_LOGI("ui_settings", "主题已切换为: %s", bsp_theme_name(new_theme));

        /* 取消之前的重建定时器 */
        if (s_theme_rebuild_timer) {
            lv_timer_del(s_theme_rebuild_timer);
            s_theme_rebuild_timer = NULL;
        }

        /* 100ms后重建页面, 让LVGL先处理完当前点击事件 */
        s_theme_rebuild_timer = lv_timer_create(rebuild_after_theme_cb, 100, NULL);
        lv_timer_set_repeat_count(s_theme_rebuild_timer, 1);
    }
}

static void cb_open_theme(lv_event_t *e)
{
    (void)e;
    if (g_scr_theme != NULL) {
        lv_obj_del(g_scr_theme);
        g_scr_theme = NULL;
    }
    create_theme_page();
    safe_screen_load(g_scr_theme, false);
}

/* ========================================================================
 * 7. 后台管理子页面 (内存监控 + PSRAM 测速 + 手动回收)
 * ======================================================================== */
static lv_timer_t *s_bg_mgr_timer = NULL;

typedef struct {
    lv_obj_t *value;
    lv_obj_t *bar;
} bg_card_t;

static bg_card_t s_bg_psram;
static bg_card_t s_bg_internal;
static bg_card_t s_bg_lvgl;
static bg_card_t s_bg_pdma;
static lv_obj_t *g_bg_bench_btn = NULL;
static lv_obj_t *g_bg_bench_lbl = NULL;
static lv_obj_t *g_bg_status_lbl = NULL;

/* 字节数 -> "x.xMB" / "xKB" 文本 */
static void bg_fmt_mb(uint32_t bytes, char *out, size_t len)
{
    snprintf(out, len, "%lu.%luMB", (unsigned long)(bytes >> 20),
             (unsigned long)(((bytes & 0xFFFFF) * 10) >> 20));
}

/* 统计卡片: 标题 + (可选)使用率条 + 多行数值标签, 返回数值标签 */
static lv_obj_t *bg_card_create(lv_obj_t *parent, const char *title,
                                bool with_bar, lv_obj_t **bar_out)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(title_lbl, COLOR_GRAY, 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_coord_t value_y = 20;
    if (bar_out) *bar_out = NULL;
    if (with_bar) {
        lv_obj_t *bar = lv_bar_create(card);
        lv_obj_set_size(bar, LV_PCT(100), 10);
        lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 22);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, COLOR_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 5, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);
        if (bar_out) *bar_out = bar;
        value_y = 40;
    }

    lv_obj_t *value_lbl = lv_label_create(card);
    lv_label_set_text(value_lbl, "-");
    lv_obj_set_style_text_font(value_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(value_lbl, COLOR_WHITE, 0);
    lv_label_set_long_mode(value_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(value_lbl, LV_PCT(100));
    lv_obj_align(value_lbl, LV_ALIGN_TOP_LEFT, 0, value_y);
    return value_lbl;
}

/* 1s 自动刷新 (LVGL 线程): 只更新数值文本与进度条, 不重建控件 */
static void bg_mgr_refresh_cb(lv_timer_t *tmr)
{
    (void)tmr;
    if (g_scr_bg_mgr == NULL) return;

    char l1[24], l2[24], l3[24], buf[128];
    mem_stats_t st;

    /* PSRAM 总览 (bsp_memory 统计) */
    if (bsp_memory_get_stats(MEM_TYPE_PSRAM, &st) == ESP_OK) {
        bg_fmt_mb(st.total - st.free, l1, sizeof(l1));
        bg_fmt_mb(st.total, l2, sizeof(l2));
        snprintf(l3, sizeof(l3), "%luKB", (unsigned long)(st.largest_block / 1024));
        snprintf(buf, sizeof(buf),
                 "已用 %s / %s (%u%%)\n最大连续块 %s\n历史最低空闲 %luKB\n碎片度 %u%%",
                 l1, l2, st.usage_pct, l3,
                 (unsigned long)(st.min_ever_free / 1024), st.frag_pct);
        lv_label_set_text(s_bg_psram.value, buf);
        if (s_bg_psram.bar) {
            lv_bar_set_value(s_bg_psram.bar, st.usage_pct, LV_ANIM_OFF);
        }
    }

    /* 内部 RAM 总览 */
    if (bsp_memory_get_stats(MEM_TYPE_INTERNAL, &st) == ESP_OK) {
        snprintf(buf, sizeof(buf),
                 "已用 %luKB / %luKB (%u%%)\n最大连续块 %luKB\n历史最低 %luKB",
                 (unsigned long)((st.total - st.free) / 1024),
                 (unsigned long)(st.total / 1024), st.usage_pct,
                 (unsigned long)(st.largest_block / 1024),
                 (unsigned long)(st.min_ever_free / 1024));
        lv_label_set_text(s_bg_internal.value, buf);
        if (s_bg_internal.bar) {
            lv_bar_set_value(s_bg_internal.bar, st.usage_pct, LV_ANIM_OFF);
        }
    }

    /* LVGL 堆: lv_mem_psram.c 自定义分配器上报整个 PSRAM 堆
     * (LVGL 与应用/BT 等其他 PSRAM 分配共享, 非仅 LVGL 自身占用) */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    if (mon.total_size > 0) {
        snprintf(buf, sizeof(buf),
                 "已用 %luKB / %luKB (%u%%)\n峰值占用 %luKB\n碎片度 %u%%",
                 (unsigned long)((mon.total_size - mon.free_size) / 1024),
                 (unsigned long)(mon.total_size / 1024),
                 mon.used_pct,
                 (unsigned long)(mon.max_used / 1024),
                 mon.frag_pct);
    } else {
        snprintf(buf, sizeof(buf), "监控不可用");
    }
    lv_label_set_text(s_bg_lvgl.value, buf);
    if (s_bg_lvgl.bar && mon.total_size > 0) {
        lv_bar_set_value(s_bg_lvgl.bar, mon.used_pct, LV_ANIM_OFF);
    }

    /* PSRAM-DMA 分区 (bsp_memory 划出的 esp-hosted 专用堆) */
    uint32_t pdma_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    uint32_t pdma_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    snprintf(buf, sizeof(buf), "空闲 %luKB\n最大连续块 %luKB",
             (unsigned long)(pdma_free / 1024),
             (unsigned long)(pdma_largest / 1024));
    lv_label_set_text(s_bg_pdma.value, buf);
}

/* ---------- PSRAM 测速 (独立任务执行, 结果经 lv_async_call 回传) ---------- */

#define BENCH_BUF_SIZE   (1024 * 1024)   /* 1MB PSRAM 测试缓冲 */
#define BENCH_ROUNDS     4               /* 每项重复轮数 */

typedef struct {
    bool     ok;
    uint32_t write_mbs;
    uint32_t read_mbs;
    uint32_t copy_mbs;
} bench_result_t;

static volatile bool s_bench_running = false;
static bench_result_t s_bench_result;

static uint32_t bench_mbs(uint32_t bytes, int64_t elapsed_us)
{
    if (elapsed_us <= 0) return 0;
    return (uint32_t)((uint64_t)bytes * 1000000ULL /
                      (uint64_t)elapsed_us / (1024UL * 1024UL));
}

/* LVGL 线程: 显示测速结果并恢复按钮 */
static void bg_bench_done_cb(void *p)
{
    (void)p;
    if (g_bg_bench_lbl) {
        char buf[96];
        if (s_bench_result.ok) {
            snprintf(buf, sizeof(buf), "写 %u MB/s | 读 %u MB/s | 复制 %u MB/s",
                     (unsigned)s_bench_result.write_mbs,
                     (unsigned)s_bench_result.read_mbs,
                     (unsigned)s_bench_result.copy_mbs);
        } else {
            snprintf(buf, sizeof(buf), "1MB 缓冲分配失败, 请先手动回收");
        }
        lv_label_set_text(g_bg_bench_lbl, buf);
    }
    if (g_bg_bench_btn) {
        lv_obj_remove_state(g_bg_bench_btn, LV_STATE_DISABLED);
        lv_obj_t *lbl = lv_obj_get_child(g_bg_bench_btn, 0);
        if (lbl) lv_label_set_text(lbl, "PSRAM 测速");
    }
}

/* 测速任务 (LVGL 线程外): 1MB 写/读/复制三向计时 */
static void psram_bench_task(void *arg)
{
    (void)arg;
    bench_result_t r = {0};
    uint32_t *buf = (uint32_t *)bsp_memory_malloc(BENCH_BUF_SIZE);

    if (buf != NULL) {
        const uint32_t words = BENCH_BUF_SIZE / sizeof(uint32_t);
        volatile uint32_t sum = 0;
        int64_t t0, t1;

        /* 写入 */
        t0 = esp_timer_get_time();
        for (int round = 0; round < BENCH_ROUNDS; round++) {
            for (uint32_t i = 0; i < words; i++) {
                buf[i] = i;
            }
        }
        t1 = esp_timer_get_time();
        r.write_mbs = bench_mbs((uint32_t)BENCH_BUF_SIZE * BENCH_ROUNDS, t1 - t0);

        /* 读取 */
        t0 = esp_timer_get_time();
        for (int round = 0; round < BENCH_ROUNDS; round++) {
            for (uint32_t i = 0; i < words; i++) {
                sum += buf[i];
            }
        }
        t1 = esp_timer_get_time();
        r.read_mbs = bench_mbs((uint32_t)BENCH_BUF_SIZE * BENCH_ROUNDS, t1 - t0);
        (void)sum;

        /* 复制 (前半 -> 后半, 512KB) */
        t0 = esp_timer_get_time();
        for (int round = 0; round < BENCH_ROUNDS; round++) {
            memcpy(buf + words / 2, buf, BENCH_BUF_SIZE / 2);
        }
        t1 = esp_timer_get_time();
        r.copy_mbs = bench_mbs((uint32_t)(BENCH_BUF_SIZE / 2) * BENCH_ROUNDS, t1 - t0);

        r.ok = true;
        free(buf);
    }

    s_bench_result = r;
    s_bench_running = false;
    ui_lvgl_async_call(bg_bench_done_cb, NULL);
    vTaskDelete(NULL);
}

static void cb_run_psram_bench(lv_event_t *e)
{
    (void)e;
    if (s_bench_running) return;
    s_bench_running = true;

    if (g_bg_bench_btn) {
        lv_obj_add_state(g_bg_bench_btn, LV_STATE_DISABLED);
        lv_obj_t *lbl = lv_obj_get_child(g_bg_bench_btn, 0);
        if (lbl) lv_label_set_text(lbl, "测速中...");
    }
    if (g_bg_bench_lbl) {
        lv_label_set_text(g_bg_bench_lbl, "正在测速 (1MB 写/读/复制)...");
    }

    xTaskCreate(psram_bench_task, "ps_bench", 4096, NULL, 2, NULL);
}

/* ---------- 手动回收 (仅常规回收, 绝不触发紧急回收) ---------- */

static void cb_trim_caches(lv_event_t *e)
{
    (void)e;
    uint32_t freed = bsp_memory_trim_caches(0);
    ESP_LOGI("ui_settings", "手动内存回收: 释放约 %u 字节", (unsigned)freed);
    if (g_bg_status_lbl) {
        char buf[64];
        snprintf(buf, sizeof(buf), "已触发回收, 约释放 %luKB (部分异步生效)",
                 (unsigned long)(freed / 1024));
        lv_label_set_text(g_bg_status_lbl, buf);
    }
}

/* ---------- 页面生命周期 ---------- */

static void bg_mgr_stop_timer(void)
{
    if (s_bg_mgr_timer) {
        lv_timer_del(s_bg_mgr_timer);
        s_bg_mgr_timer = NULL;
    }
}

static void swipe_back_from_bg_mgr(void)
{
    bg_mgr_stop_timer();
    if (g_scr_main != NULL) {
        ui_navbar_replace_top(g_scr_main, false);
    }
}

static void create_bg_mgr_page(void)
{
    g_scr_bg_mgr = create_screen();
    create_title_bar(g_scr_bg_mgr, "后台管理");
    ui_edge_swipe_add(g_scr_bg_mgr, swipe_back_from_bg_mgr);

    lv_obj_t *content = create_content_area(g_scr_bg_mgr);

    /* 视觉微调: 页面整体右移 3px, 宽度同步收窄避免右缘裁切 */
    lv_obj_t *title_bar = lv_obj_get_child(g_scr_bg_mgr, 0);
    if (title_bar) {
        lv_obj_set_x(title_bar, 3);
        lv_obj_set_width(title_bar, SCREEN_W - 3);
    }
    lv_obj_set_x(content, 3);
    lv_obj_set_width(content, SCREEN_W - 3);

    s_bg_psram.value = bg_card_create(content, "PSRAM (Octal 200MHz)",
                                      true, &s_bg_psram.bar);
    s_bg_internal.value = bg_card_create(content, "内部 RAM",
                                         true, &s_bg_internal.bar);
    s_bg_lvgl.value = bg_card_create(content, "LVGL 堆 (与系统共享 PSRAM)",
                                     true, &s_bg_lvgl.bar);
    s_bg_pdma.value = bg_card_create(content, "PSRAM-DMA 分区 (esp-hosted)",
                                     false, &s_bg_pdma.bar);

    /* PSRAM 测速 */
    g_bg_bench_btn = create_button(content, "PSRAM 测速", cb_run_psram_bench);
    g_bg_bench_lbl = lv_label_create(content);
    lv_label_set_text(g_bg_bench_lbl, "1MB 写/读/复制实测带宽");
    lv_obj_set_style_text_font(g_bg_bench_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_bg_bench_lbl, COLOR_GRAY, 0);
    lv_obj_set_width(g_bg_bench_lbl, LV_PCT(100));
    lv_obj_set_style_pad_left(g_bg_bench_lbl, 4, 0);

    /* 手动回收 */
    lv_obj_t *trim_btn = create_button(content, "手动回收内存", cb_trim_caches);
    lv_obj_set_style_bg_color(trim_btn, COLOR_CARD, 0);
    lv_obj_set_style_border_color(trim_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(trim_btn, 1, 0);
    g_bg_status_lbl = lv_label_create(content);
    lv_label_set_text(g_bg_status_lbl, "触发常规回收 (缓存/空闲协议栈)");
    lv_obj_set_style_text_font(g_bg_status_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_bg_status_lbl, COLOR_GRAY, 0);
    lv_obj_set_width(g_bg_status_lbl, LV_PCT(100));
    lv_obj_set_style_pad_left(g_bg_status_lbl, 4, 0);
}

static void cb_open_bg_mgr(lv_event_t *e)
{
    (void)e;
    if (g_scr_bg_mgr == NULL) {
        create_bg_mgr_page();
    }
    if (s_bg_mgr_timer == NULL) {
        s_bg_mgr_timer = lv_timer_create(bg_mgr_refresh_cb, 1000, NULL);
    }
    bg_mgr_refresh_cb(NULL);   /* 进入页面立即刷新一次 */
    safe_screen_load(g_scr_bg_mgr, true);
}

/* ========================================================================
 * 公开 API
 * ======================================================================== */

void ui_settings_show(void)
{
    if (g_scr_main == NULL) {
        create_settings_main();
    }
    /* 通过导航栏压栈, 使返回键可回到桌面 */
    ui_navbar_push_screen_named(g_scr_main, true, "设置");
}

lv_obj_t *ui_settings_get_screen(void)
{
    if (g_scr_main == NULL) {
        create_settings_main();
    }
    return g_scr_main;
}

/* 兼容旧API */
void ui_settings_update_wifi(bool connected, const char *ip)
{
    (void)connected;
    (void)ip;
}

void ui_settings_update_bt(bool connected, const char *name)
{
    (void)connected;
    (void)name;
}
