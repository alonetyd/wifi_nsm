/*******************************************************************************
 * 音乐播放器 UI 模块实现 - LVGL 9.x
 *
 * 屏幕: 262×928 (竖屏) 32bit
 *
 * 新版布局 (三页签 + 唱片动画):
 *   状态栏(40) | 页签栏(36) [播放 | 歌词 | 频谱]
 *   ─ 播放页: 旋转黑胶唱片 + 歌名/歌手 + 可拖动进度条 + 播放控制
 *             + 模式/收藏/睡眠/播放列表 + 音量滑块
 *   ─ 歌词页: LRC 歌词同步高亮 + 自动滚动
 *   ─ 频谱页: 24 条 FFT 频谱动画柱
 *   播放列表: 弹窗形式 (贴近主流音乐App操作习惯)
 ******************************************************************************/

#include "ui_music_player.h"
#include "ui_lvgl_async.h"
#include "bsp_board.h"
#include "bsp_brightness.h"
#include "bsp_audio.h"
#include "bsp_theme.h"
#include "ui_gesture.h"
#include "ui_navbar.h"
#include "ui_dialog.h"
#include "fft_spectrum.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp_memory.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if (esp_idf_version >= ESP_IDF_VERSION_VAL(5, 2, 0))
#include "freertos/idf_additions.h"
#endif

/* ======================== 颜色定义 (使用主题系统) ======================== */
static inline lv_color_t theme_bg(void)      { return bsp_theme_colors()->bg; }
static inline lv_color_t theme_card(void)    { return bsp_theme_colors()->card; }
static inline lv_color_t theme_primary(void) { return bsp_theme_colors()->primary; }
static inline lv_color_t theme_accent(void)  { return bsp_theme_colors()->accent; }

#define COLOR_BG        theme_bg()
#define COLOR_CARD      theme_card()
#define COLOR_PRIMARY   theme_primary()
#define COLOR_ACCENT    theme_accent()
#define COLOR_WHITE     lv_color_hex(0xffffff)
#define COLOR_GRAY      lv_color_hex(0xa0a0a0)

/* ======================== 屏幕尺寸 / 区域坐标 ======================== */
#define SCREEN_W   262
#define SCREEN_H   928

#define NAVBAR_H_UI   52                          /* 底部导航栏高度 (为布局预留空间) */

#define Y_STATUS    0       /* 状态栏   高40 */
#define Y_TAB       40      /* 页签栏   高36 */
#define CONTENT_Y   76      /* 内容区起点 */
#define CONTENT_H   (SCREEN_H - NAVBAR_H_UI - CONTENT_Y)  /* 内容区高度 ~800 */

/* ---- 播放页内部布局 ---- */
#define PLAY_VINYL_Y   82
#define PLAY_TITLE_Y   292
#define PLAY_ARTIST_Y  324
#define PLAY_PROG_Y    364
#define PLAY_CTRL_Y    412
#define PLAY_UTIL_Y    508
#define PLAY_VOL_Y     566

/* ======================== 页签 ======================== */
#define TAB_PLAY   0
#define TAB_LYRIC  1
#define TAB_SPEC   2
#define TAB_COUNT  3

/* ======================== 频谱参数 ======================== */
#define SPEC_BARS   24
#define SPEC_BAR_W  6
#define SPEC_BAR_GAP 4
#define SPEC_BAR_MAX_H 300

/* ======================== 静态状态 ======================== */
static bool           g_slider_updating = false;   /* 防止滑块回调递归 */
static bool           g_seeking = false;           /* 进度条拖动中 */
static audio_state_t  g_state = AUDIO_STATE_IDLE;
static int            g_play_index = -1;
static uint32_t       g_total_sec = 0;
static int            g_tab = TAB_PLAY;
static uint16_t       g_vinyl_angle = 0;

/* 播放列表名称缓存 (从 main.c 推送, PSRAM 分配避免挤占内部 DRAM) */
#define PLAYLIST_MAX 256
static char (*g_names)[128] = NULL;
static int   g_names_count = 0;

/* 歌词 */
#define LYRIC_MAX 300
static lv_obj_t *g_lyric_labels[LYRIC_MAX];
static int      g_lyric_count = 0;
static int      g_lyric_active = -1;

/* 前置声明: cb_play 等回调在定义之前调用 */
static void apply_audio_state(audio_state_t st);

/* ======================== UI 对象引用 ======================== */
static lv_obj_t *g_wifi_lbl;
static lv_obj_t *g_bt_lbl;
static lv_obj_t *g_time_lbl;
static lv_obj_t *g_batt_lbl;

/* 页签 */
static lv_obj_t *g_tab_btns[TAB_COUNT];
static lv_obj_t *g_pages[TAB_COUNT];

/* 播放页 */
static lv_obj_t *g_vinyl;            /* 黑胶唱片容器 */
static lv_obj_t *g_vinyl_highlight;  /* 黑胶高光 */
static lv_obj_t *g_title_lbl;
static lv_obj_t *g_artist_lbl;
static lv_obj_t *g_bar;
static lv_obj_t *g_cur_time_lbl;
static lv_obj_t *g_total_time_lbl;
static lv_obj_t *g_play_btn;
static lv_obj_t *g_play_icon;
static lv_obj_t *g_play_spinner;     /* 播放等待指示器 (BUFFERING 状态显示) */
static lv_obj_t *g_mode_lbl;
static lv_obj_t *g_fav_lbl;
static lv_obj_t *g_sleep_lbl;
static lv_obj_t *g_vol_slider;

/* 歌词页 */
static lv_obj_t *g_lyric_cont;
static lv_obj_t *g_lyric_hint;

/* 频谱页 */
static lv_obj_t *g_spec_song_lbl;
static lv_obj_t *g_spec_bars[SPEC_BARS];

/* ======================== 前置声明 ======================== */
static void cb_play(lv_event_t *e);
static void cb_prev(lv_event_t *e);
static void cb_next(lv_event_t *e);
static void cb_volume(lv_event_t *e);
static void cb_tab(lv_event_t *e);
static void cb_mode(lv_event_t *e);
static void cb_fav(lv_event_t *e);
static void cb_sleep(lv_event_t *e);
static void cb_playlist(lv_event_t *e);
static void cb_bar_press(lv_event_t *e);
static void cb_bar_release(lv_event_t *e);
static void cb_timer(lv_timer_t *t);

static void fmt_time(uint32_t sec, char *buf, size_t len);
static lv_obj_t *make_container(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                lv_coord_t w, lv_coord_t h);
static void create_status_bar(lv_obj_t *p);
static void create_tab_bar(lv_obj_t *p);
static void create_page_play(lv_obj_t *p);
static void create_page_lyric(lv_obj_t *p);
static void create_page_spec(lv_obj_t *p);
static void vinyl_draw(uint16_t angle);
static void set_page(int tab);
static void update_fav_btn(void);
static void update_mode_btn(void);
static void rebuild_lyrics(void);
static void update_lyric_highlight(uint32_t ms);
static void update_spec_bars(void);
static void open_sleep_dialog(void);
static void open_playlist_dialog(void);
static void refresh_track(int index);

/* ========================================================================
 * 辅助函数
 * ======================================================================== */

static void fmt_time(uint32_t sec, char *buf, size_t len)
{
    uint32_t m = sec / 60;
    uint32_t s = sec % 60;
    snprintf(buf, len, "%02lu:%02lu", (unsigned long)m, (unsigned long)s);
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

/* 圆形小按钮 (统一风格) */
static lv_obj_t *make_round_btn(lv_obj_t *parent, lv_coord_t size,
                                lv_event_cb_t cb, const char *text)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, size / 2, 0);
    lv_obj_set_style_bg_color(btn, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &esp_2, 0);
    lv_obj_set_style_text_color(lbl, COLOR_WHITE, 0);
    lv_obj_center(lbl);
    return btn;
}

/* ========================================================================
 * 黑胶唱片 (纯 LVGL 对象绘制，避免 Canvas/PSRAM 绘制链路)
 * ======================================================================== */
#define VINYL_SIZE 200

static void vinyl_draw(uint16_t angle)
{
    if (g_vinyl_highlight == NULL) return;
    /* 仅旋转高光对象，不进行 Canvas 重绘或大块内存写入 */
    lv_obj_set_style_transform_angle(g_vinyl_highlight, angle * 10, 0);
}

/* ========================================================================
 * UI 区域创建
 * ======================================================================== */

/* 1. 顶部状态栏 (高40) */
static void create_status_bar(lv_obj_t *p)
{
    lv_obj_t *bar = make_container(p, 0, Y_STATUS, SCREEN_W, 40);
    lv_obj_set_style_bg_color(bar, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    g_wifi_lbl = lv_label_create(bar);
    lv_label_set_text(g_wifi_lbl, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(g_wifi_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_wifi_lbl, COLOR_GRAY, 0);
    lv_obj_align(g_wifi_lbl, LV_ALIGN_LEFT_MID, 44, 0);

    g_bt_lbl = lv_label_create(bar);
    lv_label_set_text(g_bt_lbl, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(g_bt_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_bt_lbl, COLOR_GRAY, 0);
    lv_obj_align(g_bt_lbl, LV_ALIGN_LEFT_MID, 70, 0);

    g_batt_lbl = lv_label_create(bar);
    lv_label_set_text(g_batt_lbl, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_font(g_batt_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_batt_lbl, COLOR_WHITE, 0);
    lv_obj_align(g_batt_lbl, LV_ALIGN_RIGHT_MID, -8, 0);

    g_time_lbl = lv_label_create(bar);
    lv_label_set_text(g_time_lbl, "00:00");
    lv_obj_set_style_text_font(g_time_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_time_lbl, COLOR_WHITE, 0);
    lv_obj_align(g_time_lbl, LV_ALIGN_RIGHT_MID, -34, 0);
}

/* 2. 页签栏 (高36): [播放 | 歌词 | 频谱] */
static void create_tab_bar(lv_obj_t *p)
{
    lv_obj_t *bar = make_container(p, 0, Y_TAB, SCREEN_W, 36);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x171827), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(bar, 6, 0);
    lv_obj_set_style_pad_right(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const char *tab_text[TAB_COUNT] = {"播放", "歌词", "频谱"};
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *btn = lv_btn_create(bar);
        lv_obj_set_size(btn, SCREEN_W / TAB_COUNT - 4, 28);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb_tab, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, tab_text[i]);
        lv_obj_set_style_text_font(lbl, &esp_2, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x8a86b8), 0);
        lv_obj_center(lbl);
        g_tab_btns[i] = btn;
    }
}

/* 3. 播放页 */
static void create_page_play(lv_obj_t *p)
{
    lv_obj_t *page = make_container(p, 0, CONTENT_Y, SCREEN_W, CONTENT_H);
    g_pages[TAB_PLAY] = page;

    /* ---- 黑胶唱片 (纯 LVGL 对象，无 Canvas/PSRAM 缓冲) ---- */
    g_vinyl = lv_obj_create(page);
    lv_obj_set_size(g_vinyl, VINYL_SIZE, VINYL_SIZE);
    lv_obj_set_pos(g_vinyl, (SCREEN_W - VINYL_SIZE) / 2, PLAY_VINYL_Y);
    lv_obj_set_style_bg_color(g_vinyl, lv_color_hex(0x10111c), 0);
    lv_obj_set_style_bg_opa(g_vinyl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_vinyl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(g_vinyl, 6, 0);
    lv_obj_set_style_border_color(g_vinyl, lv_color_hex(0x252742), 0);
    lv_obj_set_style_shadow_width(g_vinyl, 18, 0);
    lv_obj_set_style_shadow_opa(g_vinyl, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(g_vinyl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(g_vinyl, 0, 0);
    lv_obj_clear_flag(g_vinyl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_vinyl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_vinyl, cb_play, LV_EVENT_CLICKED, NULL);

    /* 唱片纹路用少量圆形对象表示，避免大块 Canvas 内存 */
    for (int r = 86; r >= 42; r -= 11) {
        lv_obj_t *ring = lv_obj_create(g_vinyl);
        lv_obj_set_size(ring, r * 2, r * 2);
        lv_obj_center(ring);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring,
            (r & 1) ? lv_color_hex(0x30324d) : lv_color_hex(0x202238), 0);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *center = lv_obj_create(g_vinyl);
    lv_obj_set_size(center, 60, 60);
    lv_obj_center(center);
    lv_obj_set_style_bg_color(center, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(center, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(center, 3, 0);
    lv_obj_set_style_border_color(center, lv_color_hex(0xf0efff), 0);
    lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(center, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hole = lv_obj_create(center);
    lv_obj_set_size(hole, 12, 12);
    lv_obj_center(hole);
    lv_obj_set_style_bg_color(hole, lv_color_hex(0x14141e), 0);
    lv_obj_set_style_bg_opa(hole, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hole, 0, 0);
    lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(hole, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    g_vinyl_highlight = lv_obj_create(g_vinyl);
    lv_obj_set_size(g_vinyl_highlight, 8, 70);
    lv_obj_set_pos(g_vinyl_highlight, VINYL_SIZE / 2 - 4, 16);
    lv_obj_set_style_bg_color(g_vinyl_highlight, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(g_vinyl_highlight, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_vinyl_highlight, 0, 0);
    lv_obj_set_style_radius(g_vinyl_highlight, 4, 0);
    lv_obj_set_style_transform_pivot_x(g_vinyl_highlight, 4, 0);
    lv_obj_set_style_transform_pivot_y(g_vinyl_highlight, 35, 0);
    lv_obj_clear_flag(g_vinyl_highlight, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    vinyl_draw(0);

    /* ---- 歌名 (滚动) ---- */
    g_title_lbl = lv_label_create(page);
    lv_label_set_text(g_title_lbl, "未播放");
    lv_obj_set_style_text_font(g_title_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_title_lbl, lv_color_hex(0xf0efff), 0);
    lv_obj_set_style_text_letter_space(g_title_lbl, 1, 0);
    lv_label_set_long_mode(g_title_lbl, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(g_title_lbl, 240);
    lv_obj_align(g_title_lbl, LV_ALIGN_TOP_MID, 0, PLAY_TITLE_Y);

    /* ---- 歌手 ---- */
    g_artist_lbl = lv_label_create(page);
    lv_label_set_text(g_artist_lbl, "--");
    lv_obj_set_style_text_font(g_artist_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_artist_lbl, lv_color_hex(0x8a86b8), 0);
    lv_obj_set_style_text_letter_space(g_artist_lbl, 1, 0);
    lv_label_set_long_mode(g_artist_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(g_artist_lbl, 240);
    lv_obj_align(g_artist_lbl, LV_ALIGN_TOP_MID, 0, PLAY_ARTIST_Y);

    /* ---- 进度条行 ---- */
    lv_obj_t *prog = make_container(page, 6, PLAY_PROG_Y, SCREEN_W - 12, 40);
    lv_obj_set_flex_flow(prog, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(prog, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(prog, 8, 0);

    g_cur_time_lbl = lv_label_create(prog);
    lv_label_set_text(g_cur_time_lbl, "00:00");
    lv_obj_set_style_text_font(g_cur_time_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_cur_time_lbl, COLOR_GRAY, 0);

    g_bar = lv_bar_create(prog);
    lv_obj_set_height(g_bar, 6);
    lv_obj_set_flex_grow(g_bar, 1);
    lv_bar_set_range(g_bar, 0, 100);
    lv_bar_set_value(g_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_bar, lv_color_hex(0x363653), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bar, lv_color_hex(0x569af8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(g_bar, lv_color_hex(0xa666f1), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(g_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_bar, 3, LV_PART_INDICATOR);
    /* 拖动 seek */
    lv_obj_add_event_cb(g_bar, cb_bar_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_bar, cb_bar_press, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(g_bar, cb_bar_release, LV_EVENT_RELEASED, NULL);

    g_total_time_lbl = lv_label_create(prog);
    lv_label_set_text(g_total_time_lbl, "00:00");
    lv_obj_set_style_text_font(g_total_time_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_total_time_lbl, COLOR_GRAY, 0);

    /* ---- 播放控制 ---- */
    lv_obj_t *ctrl = make_container(page, 0, PLAY_CTRL_Y, SCREEN_W, 92);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *prev_btn = make_round_btn(ctrl, 44, cb_prev, LV_SYMBOL_PREV);
    (void)prev_btn;

    g_play_btn = lv_btn_create(ctrl);
    lv_obj_set_size(g_play_btn, 64, 64);
    lv_obj_set_style_radius(g_play_btn, 32, 0);
    lv_obj_set_style_bg_color(g_play_btn, lv_color_hex(0x6f8af6), 0);
    lv_obj_set_style_bg_grad_color(g_play_btn, lv_color_hex(0xa666f1), 0);
    lv_obj_set_style_bg_grad_dir(g_play_btn, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(g_play_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(g_play_btn, 14, 0);
    lv_obj_set_style_shadow_opa(g_play_btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(g_play_btn, lv_color_hex(0x6f8af6), 0);
    lv_obj_set_style_border_width(g_play_btn, 0, 0);
    lv_obj_add_event_cb(g_play_btn, cb_play, LV_EVENT_CLICKED, NULL);
    g_play_icon = lv_label_create(g_play_btn);
    lv_label_set_text(g_play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(g_play_icon, &esp_2, 0);
    lv_obj_set_style_text_color(g_play_icon, COLOR_WHITE, 0);
    lv_obj_center(g_play_icon);

    /* 播放等待指示器: 准备中(BUFFERING)时替代播放图标旋转 */
    g_play_spinner = lv_spinner_create(g_play_btn);
    if (g_play_spinner) {
        lv_obj_set_size(g_play_spinner, 40, 40);
        lv_obj_center(g_play_spinner);
        lv_obj_set_style_arc_width(g_play_spinner, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_color(g_play_spinner, COLOR_WHITE, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(g_play_spinner, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_arc_width(g_play_spinner, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(g_play_spinner, COLOR_WHITE, LV_PART_INDICATOR);
        lv_obj_set_style_anim_duration(g_play_spinner, 900, 0);
        lv_obj_add_flag(g_play_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_play_spinner, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *next_btn = make_round_btn(ctrl, 44, cb_next, LV_SYMBOL_NEXT);
    (void)next_btn;

    /* ---- 工具行: 模式 | 收藏 | 睡眠 | 播放列表 ---- */
    lv_obj_t *util = make_container(page, 0, PLAY_UTIL_Y, SCREEN_W, 54);
    lv_obj_set_flex_flow(util, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(util, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *mode_btn = lv_btn_create(util);
    lv_obj_set_size(mode_btn, 56, 44);
    lv_obj_set_style_radius(mode_btn, 10, 0);
    lv_obj_set_style_bg_color(mode_btn, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(mode_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(mode_btn, 0, 0);
    lv_obj_set_style_border_width(mode_btn, 0, 0);
    lv_obj_add_event_cb(mode_btn, cb_mode, LV_EVENT_CLICKED, NULL);
    g_mode_lbl = lv_label_create(mode_btn);
    lv_obj_set_style_text_font(g_mode_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_mode_lbl, COLOR_WHITE, 0);
    lv_obj_center(g_mode_lbl);

    lv_obj_t *fav_btn = lv_btn_create(util);
    lv_obj_set_size(fav_btn, 56, 44);
    lv_obj_set_style_radius(fav_btn, 10, 0);
    lv_obj_set_style_bg_color(fav_btn, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(fav_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(fav_btn, 0, 0);
    lv_obj_set_style_border_width(fav_btn, 0, 0);
    lv_obj_add_event_cb(fav_btn, cb_fav, LV_EVENT_CLICKED, NULL);
    g_fav_lbl = lv_label_create(fav_btn);
    lv_label_set_text(g_fav_lbl, "收藏");
    lv_obj_set_style_text_font(g_fav_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_fav_lbl, COLOR_WHITE, 0);
    lv_obj_center(g_fav_lbl);

    lv_obj_t *sleep_btn = lv_btn_create(util);
    lv_obj_set_size(sleep_btn, 56, 44);
    lv_obj_set_style_radius(sleep_btn, 10, 0);
    lv_obj_set_style_bg_color(sleep_btn, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(sleep_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(sleep_btn, 0, 0);
    lv_obj_set_style_border_width(sleep_btn, 0, 0);
    lv_obj_add_event_cb(sleep_btn, cb_sleep, LV_EVENT_CLICKED, NULL);
    g_sleep_lbl = lv_label_create(sleep_btn);
    lv_label_set_text(g_sleep_lbl, "睡眠");
    lv_obj_set_style_text_font(g_sleep_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_sleep_lbl, COLOR_WHITE, 0);
    lv_obj_center(g_sleep_lbl);

    lv_obj_t *list_btn = lv_btn_create(util);
    lv_obj_set_size(list_btn, 56, 44);
    lv_obj_set_style_radius(list_btn, 10, 0);
    lv_obj_set_style_bg_color(list_btn, lv_color_hex(0x242642), 0);
    lv_obj_set_style_bg_opa(list_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(list_btn, 0, 0);
    lv_obj_set_style_border_width(list_btn, 0, 0);
    lv_obj_add_event_cb(list_btn, cb_playlist, LV_EVENT_CLICKED, NULL);
    lv_obj_t *list_lbl = lv_label_create(list_btn);
    lv_label_set_text(list_lbl, LV_SYMBOL_LIST);
    lv_obj_set_style_text_color(list_lbl, lv_color_hex(0xf0efff), 0);
    lv_obj_set_style_text_font(list_lbl, &esp_2, 0);
    lv_obj_center(list_lbl);

    /* ---- 音量行 ---- */
    lv_obj_t *vol = make_container(page, 8, PLAY_VOL_Y, SCREEN_W - 16, 40);
    lv_obj_set_flex_flow(vol, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vol, 8, 0);

    lv_obj_t *vol_icon = lv_label_create(vol);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(vol_icon, &esp_2, 0);
    lv_obj_set_style_text_color(vol_icon, COLOR_WHITE, 0);

    g_vol_slider = lv_slider_create(vol);
    lv_obj_set_height(g_vol_slider, 6);
    lv_obj_set_flex_grow(g_vol_slider, 1);
    lv_slider_set_range(g_vol_slider, 0, 100);
    lv_slider_set_value(g_vol_slider, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_vol_slider, lv_color_hex(0x363653), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_vol_slider, lv_color_hex(0x569af8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(g_vol_slider, lv_color_hex(0xa666f1), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(g_vol_slider, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_vol_slider, lv_color_hex(0xf0efff), LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_vol_slider, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(g_vol_slider, cb_volume, LV_EVENT_VALUE_CHANGED, NULL);
}

/* 4. 歌词页 */
static void create_page_lyric(lv_obj_t *p)
{
    lv_obj_t *page = make_container(p, 0, CONTENT_Y, SCREEN_W, CONTENT_H);
    g_pages[TAB_LYRIC] = page;

    g_lyric_cont = lv_obj_create(page);
    lv_obj_set_size(g_lyric_cont, SCREEN_W - 20, CONTENT_H - 16);
    lv_obj_set_pos(g_lyric_cont, 10, 8);
    lv_obj_set_style_bg_opa(g_lyric_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_lyric_cont, 0, 0);
    lv_obj_set_style_radius(g_lyric_cont, 0, 0);
    lv_obj_set_flex_flow(g_lyric_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_lyric_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_lyric_cont, 6, 0);
    lv_obj_set_scroll_dir(g_lyric_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_lyric_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(g_lyric_cont, LV_OBJ_FLAG_SCROLL_ELASTIC);

    g_lyric_hint = lv_label_create(g_lyric_cont);
    lv_label_set_text(g_lyric_hint, "暂无歌词");
    lv_obj_set_style_text_font(g_lyric_hint, &esp_2, 0);
    lv_obj_set_style_text_color(g_lyric_hint, COLOR_GRAY, 0);
}

/* 5. 频谱页 */
static void create_page_spec(lv_obj_t *p)
{
    lv_obj_t *page = make_container(p, 0, CONTENT_Y, SCREEN_W, CONTENT_H);
    g_pages[TAB_SPEC] = page;

    lv_mem_monitor_t mm;
    lv_mem_monitor(&mm);
    ESP_LOGI("MUSIC", "spec begin: lvgl free=%d", (int)mm.free_size);

    /* 当前歌曲 */
    g_spec_song_lbl = lv_label_create(page);
    lv_label_set_text(g_spec_song_lbl, "--");
    lv_obj_set_style_text_font(g_spec_song_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_spec_song_lbl, COLOR_WHITE, 0);
    lv_label_set_long_mode(g_spec_song_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(g_spec_song_lbl, 240);
    lv_obj_align(g_spec_song_lbl, LV_ALIGN_TOP_MID, 0, 16);

    /* 频谱条容器 (条形底部对齐, 向上生长) */
    lv_obj_t *cont = make_container(page, 10, 60, SCREEN_W - 20, SPEC_BAR_MAX_H);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 0, 0);

    for (int i = 0; i < SPEC_BARS; i++) {
        lv_obj_t *bar = lv_obj_create(cont);
        if (bar == NULL) {
            lv_mem_monitor(&mm);
            ESP_LOGE("MUSIC", "spec: bar %d ALLOC FAIL, lvgl free=%d", i, (int)mm.free_size);
            break;
        }
        lv_obj_set_size(bar, SPEC_BAR_W, 4);
        lv_obj_set_style_radius(bar, SPEC_BAR_W / 2, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        /* 颜色: 低 → 高 由 primary → accent 渐变 */
        lv_obj_set_style_bg_color(bar,
            lv_color_mix(COLOR_ACCENT, COLOR_PRIMARY,
                         (lv_opa_t)(i * 255 / (SPEC_BARS - 1))), 0);
        g_spec_bars[i] = bar;
        if ((i & 3) == 0) {
            lv_mem_monitor(&mm);
            ESP_LOGI("MUSIC", "spec bar %d: lvgl free=%d", i, (int)mm.free_size);
        }
    }

    /* 底部文字 */
    lv_obj_t *tip = lv_label_create(page);
    lv_label_set_text(tip, "频谱");
    lv_obj_set_style_text_font(tip, &esp_2, 0);
    lv_obj_set_style_text_color(tip, COLOR_GRAY, 0);
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -24);
}

/* ========================================================================
 * 页签切换
 * ======================================================================== */
static void set_page(int tab)
{
    if (tab < 0 || tab >= TAB_COUNT) return;
    g_tab = tab;

    for (int i = 0; i < TAB_COUNT; i++) {
        bool active = (i == tab);
        if (active) {
            lv_obj_clear_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
        /* 页签按钮样式 */
        if (active) {
            lv_obj_set_style_bg_color(g_tab_btns[i], COLOR_ACCENT, 0);
        } else {
            lv_obj_set_style_bg_color(g_tab_btns[i], COLOR_CARD, 0);
        }
    }

    if (tab == TAB_PLAY) {
        vinyl_draw(g_vinyl_angle);
    } else if (tab == TAB_SPEC) {
        update_spec_bars();
    }
}

/* ========================================================================
 * 回调
 * ======================================================================== */

static void cb_tab(lv_event_t *e)
{
    int tab = (int)(intptr_t)lv_event_get_user_data(e);
    set_page(tab);
}

static void cb_play(lv_event_t *e)
{
    (void)e;
    /* 准备中: 忽略点击, 等待解码器就绪 (避免与切歌流程竞态) */
    if (g_state == AUDIO_STATE_BUFFERING) {
        return;
    }
    if (g_state == AUDIO_STATE_PLAYING) {
        audio_player_pause();
        apply_audio_state(AUDIO_STATE_PAUSED);
    } else {
        audio_player_play();
        /* 状态回调稍后经 lv_async 到达; 此处先本地预更新, 响应更即时 */
        apply_audio_state(audio_player_get_state());
    }
}

static void cb_prev(lv_event_t *e) { (void)e; audio_player_prev(); }
static void cb_next(lv_event_t *e) { (void)e; audio_player_next(); }

static void cb_volume(lv_event_t *e)
{
    if (g_slider_updating) return;
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    audio_player_set_volume((uint8_t)lv_slider_get_value(slider));
}

/* 播放模式循环切换 */
static void cb_mode(lv_event_t *e)
{
    (void)e;
    play_mode_t m = audio_player_get_mode();
    m = (play_mode_t)(((int)m + 1) % 4);
    audio_player_set_mode(m);
    update_mode_btn();
}

static void update_mode_btn(void)
{
    static const char *txt[4] = {"顺序", "随机", "单曲", "循环"};
    play_mode_t m = audio_player_get_mode();
    if (g_mode_lbl) lv_label_set_text(g_mode_lbl, txt[(int)m]);
}

static void cb_fav(lv_event_t *e)
{
    (void)e;
    if (g_play_index < 0) return;
    audio_player_toggle_favorite(g_play_index);
    update_fav_btn();
}

static void update_fav_btn(void)
{
    if (!g_fav_lbl) return;
    bool fav = (g_play_index >= 0) && audio_player_is_favorite(g_play_index);
    lv_label_set_text(g_fav_lbl, fav ? "已藏" : "收藏");
    lv_obj_set_style_text_color(g_fav_lbl,
                                fav ? COLOR_ACCENT : COLOR_WHITE, 0);
}

/* 进度条拖动: 实时预览位置 */
static void cb_bar_press(lv_event_t *e)
{
    lv_obj_t *bar = (lv_obj_t *)lv_event_get_target(e);
    g_seeking = true;

    lv_indev_t *indev = lv_indev_active();
    if (!indev || g_total_sec == 0) return;

    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_area_t area;
    lv_obj_get_coords(bar, &area);
    lv_coord_t w = area.x2 - area.x1;
    if (w <= 0) return;

    int32_t x = pt.x - area.x1;
    if (x < 0) x = 0;
    if (x > w) x = w;

    uint32_t sec = (uint32_t)((int64_t)x * g_total_sec / w);
    lv_bar_set_value(bar, (int32_t)(sec * 100UL / g_total_sec), LV_ANIM_OFF);
    char buf[16];
    fmt_time(sec, buf, sizeof(buf));
    lv_label_set_text(g_cur_time_lbl, buf);
}

/* 进度条松手: 执行 seek */
static void cb_bar_release(lv_event_t *e)
{
    lv_obj_t *bar = (lv_obj_t *)lv_event_get_target(e);
    g_seeking = false;
    if (g_total_sec == 0) return;

    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_area_t area;
    lv_obj_get_coords(bar, &area);
    lv_coord_t w = area.x2 - area.x1;
    if (w <= 0) return;

    int32_t x = pt.x - area.x1;
    if (x < 0) x = 0;
    if (x > w) x = w;

    uint32_t sec = (uint32_t)((int64_t)x * g_total_sec / w);
    audio_player_seek(sec);
    char buf[16];
    fmt_time(sec, buf, sizeof(buf));
    lv_label_set_text(g_cur_time_lbl, buf);
}

/* ========================================================================
 * 睡眠定时对话框
 * ======================================================================== */

static ui_dialog_t *g_sleep_dlg = NULL;

/* 睡眠选项回调 (minutes 通过 user_data 传递, -1 = 取消) */
static void cb_sleep_pick(lv_event_t *e)
{
    int minutes = (int)(intptr_t)lv_event_get_user_data(e);
    audio_player_set_sleep((uint32_t)(minutes < 0 ? 0 : minutes));
    if (g_sleep_dlg) {
        ui_dialog_close(g_sleep_dlg, true);
        g_sleep_dlg = NULL;
    }
}

static void cb_sleep(lv_event_t *e)
{
    (void)e;
    open_sleep_dialog();
}

static void open_sleep_dialog(void)
{
    if (g_sleep_dlg) return;

    ui_dialog_t *dlg = ui_dialog_create(NULL);
    if (!dlg) return;
    g_sleep_dlg = dlg;

    ui_dialog_set_title(dlg, "睡眠定时");
    ui_dialog_set_size(dlg, 220, 320);
    ui_dialog_set_mask_click_close(dlg, true);

    lv_obj_t *content = ui_dialog_get_content(dlg);

    static const int options[] = {15, 30, 45, 60, -1};  /* -1 = 取消 */
    static const char *labels[] = {"15 分钟", "30 分钟", "45 分钟", "60 分钟", "取消"};

    for (int i = 0; i < 5; i++) {
        lv_obj_t *btn = lv_btn_create(content);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, 42);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        if (options[i] < 0) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a3a4a), 0);
        } else {
            lv_obj_set_style_bg_color(btn, COLOR_CARD, 0);
        }
        lv_obj_add_event_cb(btn, cb_sleep_pick, LV_EVENT_CLICKED,
                            (void *)(intptr_t)options[i]);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, labels[i]);
        lv_obj_set_style_text_font(lbl, &esp_2, 0);
        lv_obj_set_style_text_color(lbl, COLOR_WHITE, 0);
        lv_obj_center(lbl);
    }

    ui_dialog_show(dlg);
}

/* ========================================================================
 * 播放列表对话框
 * ======================================================================== */

static ui_dialog_t *g_playlist_dlg = NULL;

/* 播放列表项点击: 播放该曲并关闭弹窗 */
static void cb_playlist_pick(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    audio_player_play_index(index);
    if (g_playlist_dlg) {
        ui_dialog_close(g_playlist_dlg, true);
        g_playlist_dlg = NULL;
    }
}

static void cb_playlist(lv_event_t *e)
{
    (void)e;
    open_playlist_dialog();
}

static void open_playlist_dialog(void)
{
    if (g_playlist_dlg) return;

    ui_dialog_t *dlg = ui_dialog_create(NULL);
    if (!dlg) return;
    g_playlist_dlg = dlg;

    ui_dialog_set_title(dlg, "播放列表");
    ui_dialog_set_size(dlg, 250, 660);
    ui_dialog_set_mask_click_close(dlg, true);

    lv_obj_t *content = ui_dialog_get_content(dlg);
    lv_obj_set_style_pad_row(content, 2, 0);

    if (g_names_count <= 0) {
        lv_obj_t *hint = lv_label_create(content);
        /* 开机后台扫描进行中: 提示而不是"列表为空" */
        lv_label_set_text(hint, audio_player_is_scanning()
                                ? "正在扫描音乐..." : "播放列表为空");
        lv_obj_set_style_text_font(hint, &esp_2, 0);
        lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
    } else {
        lv_obj_t *list = lv_list_create(content);
        lv_obj_set_width(list, lv_pct(100));
        lv_obj_set_height(list, lv_pct(100));
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_style_radius(list, 0, 0);
        lv_obj_set_style_pad_all(list, 0, 0);
        lv_obj_set_style_text_font(list, &esp_2, 0);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

        for (int i = 0; i < g_names_count; i++) {
            lv_obj_t *btn = lv_list_add_button(list, NULL, g_names[i]);
            lv_obj_set_style_text_font(btn, &esp_2, 0);
            lv_obj_add_event_cb(btn, cb_playlist_pick, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
            if (i == g_play_index) {
                lv_obj_set_style_bg_color(btn, COLOR_ACCENT, 0);
                lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
                lv_obj_set_style_text_color(btn, COLOR_WHITE, 0);
                lv_obj_scroll_to_view(btn, LV_ANIM_OFF);
            } else {
                lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
                lv_obj_set_style_text_color(btn, COLOR_GRAY, 0);
            }
        }
    }

    ui_dialog_show(dlg);
}

/* ========================================================================
 * 歌词
 * ======================================================================== */

static void rebuild_lyrics(void)
{
    if (!g_lyric_cont) return;
    lv_obj_clean(g_lyric_cont);
    g_lyric_count = audio_player_get_lyric_count();
    g_lyric_active = -1;

    if (g_lyric_count <= 0) {
        lv_obj_t *hint = lv_label_create(g_lyric_cont);
        lv_label_set_text(hint, "暂无歌词");
        lv_obj_set_style_text_font(hint, &esp_2, 0);
        lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
        return;
    }

    for (int i = 0; i < g_lyric_count && i < LYRIC_MAX; i++) {
        lv_obj_t *lbl = lv_label_create(g_lyric_cont);
        lv_label_set_text(lbl, audio_player_get_lyric_text(i));
        lv_obj_set_style_text_font(lbl, &esp_2, 0);
        lv_obj_set_style_text_color(lbl, COLOR_GRAY, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(lbl, SCREEN_W - 40);
        g_lyric_labels[i] = lbl;
    }
    g_lyric_count = (g_lyric_count < LYRIC_MAX) ? g_lyric_count : LYRIC_MAX;
}

static void update_lyric_highlight(uint32_t ms)
{
    if (!g_lyric_cont || g_lyric_count <= 0) return;

    int line = audio_player_get_lyric_line(ms);
    if (line < 0) line = -1;
    if (line == g_lyric_active) return;

    /* 复位旧行 */
    if (g_lyric_active >= 0 && g_lyric_active < g_lyric_count) {
        lv_obj_set_style_text_color(g_lyric_labels[g_lyric_active], COLOR_GRAY, 0);
    }

    g_lyric_active = line;

    /* 高亮新行 + 滚动到中间 */
    if (line >= 0 && line < g_lyric_count) {
        lv_obj_t *lbl = g_lyric_labels[line];
        lv_obj_set_style_text_color(lbl, COLOR_ACCENT, 0);
        lv_obj_set_style_text_font(lbl, &esp_2, 0);

        lv_coord_t ly = lv_obj_get_y(lbl);
        lv_coord_t lh = lv_obj_get_height(lbl);
        lv_coord_t target = ly - (CONTENT_H - 16) / 2 + lh / 2;
        if (target < 0) target = 0;
        lv_obj_scroll_to_y(g_lyric_cont, target, LV_ANIM_ON);
    }
}

/* ========================================================================
 * 频谱
 * ======================================================================== */

static void update_spec_bars(void)
{
    fft_spectrum_compute();
    for (int i = 0; i < SPEC_BARS; i++) {
        uint8_t v = fft_spectrum_get_bar(i);
        int h = 4 + (int)v * (SPEC_BAR_MAX_H - 4) / 255;
        lv_obj_set_height(g_spec_bars[i], h);
    }
}

/* ========================================================================
 * 进度回调 (audio_player 任务线程) -> lv_async_call 中转到 LVGL 线程
 * ======================================================================== */

typedef struct {
    int      index;
    uint32_t cur;
    uint32_t total;
    char    *title;
} progress_msg_t;

/* 静态环形消息池: 固定 8 条, 避免每次进度回调都做小块 heap 分配.
 * 生产者 = audio_player 任务 (单线程), 消费者 = LVGL 线程 (单线程),
 * 槽位标志为单字节读写, 跨线程安全. 池满时丢弃本次进度刷新. */
#define MSG_POOL_LEN 8

static progress_msg_t s_progress_pool[MSG_POOL_LEN];
static volatile bool s_progress_used[MSG_POOL_LEN];
static int s_progress_next = 0;

static progress_msg_t *progress_msg_alloc(void)
{
    for (int i = 0; i < MSG_POOL_LEN; i++) {
        int idx = (s_progress_next + i) % MSG_POOL_LEN;
        if (!s_progress_used[idx]) {
            s_progress_used[idx] = true;
            s_progress_next = (idx + 1) % MSG_POOL_LEN;
            return &s_progress_pool[idx];
        }
    }
    return NULL;
}

static void progress_msg_release(progress_msg_t *msg)
{
    if (msg && msg >= s_progress_pool && msg < s_progress_pool + MSG_POOL_LEN) {
        s_progress_used[msg - s_progress_pool] = false;
    }
}

static void progress_async_cb(void *p)
{
    progress_msg_t *msg = (progress_msg_t *)p;
    if (!msg) return;

    if (g_state != AUDIO_STATE_PAUSED) {
        g_state = AUDIO_STATE_PLAYING;
    }
    lv_label_set_text(g_play_icon, LV_SYMBOL_PAUSE);

    if (!g_seeking) {
        lv_bar_set_value(g_bar, msg->total ? (int32_t)(msg->cur * 100UL / msg->total) : 0,
                         LV_ANIM_OFF);
        char buf[16];
        fmt_time(msg->cur, buf, sizeof(buf));
        lv_label_set_text(g_cur_time_lbl, buf);
    }
    char buf[16];
    fmt_time(msg->total, buf, sizeof(buf));
    lv_label_set_text(g_total_time_lbl, buf);
    g_total_sec = msg->total;

    if (msg->title && msg->title[0]) {
        lv_label_set_text(g_title_lbl, msg->title);
        lv_label_set_text(g_spec_song_lbl, msg->title);
    }

    /* 歌词高亮 */
    update_lyric_highlight(msg->cur * 1000U);

    if (msg->title) free(msg->title);
    progress_msg_release(msg);
}

static void ui_progress_cb(int index, uint32_t cur, uint32_t total, const char *title)
{
    progress_msg_t *msg = progress_msg_alloc();
    if (!msg) return;
    msg->index = index;
    msg->cur = cur;
    msg->total = total;
    msg->title = NULL;
    if (title) {
        size_t len = strlen(title);
        if (len < 256) {
            msg->title = (char *)bsp_memory_malloc(len + 1);
            if (msg->title) memcpy(msg->title, title, len + 1);
        }
    }
    ui_lvgl_async_call(progress_async_cb, msg);
}

/* 曲目开始回调 -> 刷新 UI (歌名/歌手/歌词/收藏/列表高亮) */
typedef struct {
    int index;
} track_msg_t;

/* 静态环形消息池 (同 progress 池模式) */
static track_msg_t s_track_pool[MSG_POOL_LEN];
static volatile bool s_track_used[MSG_POOL_LEN];
static int s_track_next = 0;

static track_msg_t *track_msg_alloc(void)
{
    for (int i = 0; i < MSG_POOL_LEN; i++) {
        int idx = (s_track_next + i) % MSG_POOL_LEN;
        if (!s_track_used[idx]) {
            s_track_used[idx] = true;
            s_track_next = (idx + 1) % MSG_POOL_LEN;
            return &s_track_pool[idx];
        }
    }
    return NULL;
}

static void track_msg_release(track_msg_t *msg)
{
    if (msg && msg >= s_track_pool && msg < s_track_pool + MSG_POOL_LEN) {
        s_track_used[msg - s_track_pool] = false;
    }
}

static void track_async_cb(void *p)
{
    track_msg_t *msg = (track_msg_t *)p;
    if (!msg) return;
    refresh_track(msg->index);
    track_msg_release(msg);
}

static void ui_track_cb(int index)
{
    track_msg_t *msg = track_msg_alloc();
    if (!msg) return;
    msg->index = index;
    ui_lvgl_async_call(track_async_cb, msg);
}

/* ========================================================================
 * 歌词后台加载
 *
 * SD 读取 + LRC 解析耗时 (几十~几百 ms), 旧版在 LVGL 线程的 refresh_track
 * 里同步做, 切歌瞬间 UI 卡顿。现改为: 后台任务加载, 完成后 lv_async 回
 * LVGL 线程重建 label; "latest wins" — 快速连切时只采用最后一次请求。
 * ======================================================================== */

typedef struct {
    int index;   /* 完成加载的曲目索引 */
    int seq;     /* 对应的请求序号 (过期请求的结果直接丢弃) */
} lyric_msg_t;

static TaskHandle_t s_lyric_task = NULL;
static volatile int s_lyric_req_index   = -1;  /* 待加载索引 (worker 读取) */
static volatile int s_lyric_seq         = 0;   /* 请求序号 */
static volatile int s_lyric_loaded_index = -1; /* 已完成加载的索引 (缓存判定) */
static lyric_msg_t s_lyric_msg;                /* 单槽完成消息 (worker 单线程生产) */

static void lyric_ready_async_cb(void *p)
{
    lyric_msg_t *msg = (lyric_msg_t *)p;
    if (!msg) return;
    /* 过期结果 (期间又切了歌) 或与当前曲目不符: 丢弃 */
    if (msg->seq != s_lyric_seq || msg->index != g_play_index) return;
    if (msg->index < 0) return;

    s_lyric_loaded_index = msg->index;
    rebuild_lyrics();
    update_lyric_highlight(0);
}

static void lyric_load_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int idx = s_lyric_req_index;
        if (idx >= 0) {
            audio_player_load_lyrics(idx);   /* SD 读 + 解析 (audio_player 内部缓冲) */
        }
        s_lyric_msg.index = idx;
        s_lyric_msg.seq = s_lyric_seq;
        ui_lvgl_async_call(lyric_ready_async_cb, &s_lyric_msg);
    }
}

/* 请求后台加载歌词 (LVGL 线程调用); 任务创建失败时回退同步加载 */
static void lyric_request_load(int index)
{
    if (s_lyric_task == NULL) {
        BaseType_t ok = pdFAIL;
#if (esp_idf_version >= ESP_IDF_VERSION_VAL(5, 2, 0))
        ok = xTaskCreateWithCaps(lyric_load_task, "lyric_load",
                                 (8 * 1024) / sizeof(StackType_t), NULL, 3,
                                 &s_lyric_task,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) {
            ESP_LOGW("MUSIC", "PSRAM 歌词任务栈分配失败, 回退内部 RAM");
        }
#endif
        if (ok != pdPASS) {
            ok = xTaskCreate(lyric_load_task, "lyric_load",
                             (8 * 1024) / sizeof(StackType_t), NULL, 3,
                             &s_lyric_task);
        }
        if (ok != pdPASS) {
            s_lyric_task = NULL;
        }
    }

    s_lyric_req_index = index;
    s_lyric_seq++;
    if (s_lyric_task) {
        xTaskNotifyGive(s_lyric_task);
    } else {
        /* 回退: 同步加载 (极端内存不足场景) */
        audio_player_load_lyrics(index);
        s_lyric_loaded_index = index;
        rebuild_lyrics();
        update_lyric_highlight(0);
    }
}

/* 歌词区显示一条提示文本 (加载中/暂无) */
static void show_lyric_hint(const char *txt)
{
    if (!g_lyric_cont) return;
    lv_obj_clean(g_lyric_cont);
    lv_obj_t *hint = lv_label_create(g_lyric_cont);
    lv_label_set_text(hint, txt);
    lv_obj_set_style_text_font(hint, &esp_2, 0);
    lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
}

static void refresh_track(int index)
{
    g_play_index = index;

    const char *title = (index >= 0) ? audio_player_get_title(index) : "";
    const char *artist = (index >= 0) ? audio_player_get_artist(index) : "";
    lv_label_set_text(g_title_lbl, title[0] ? title : "未播放");
    lv_label_set_text(g_artist_lbl, artist[0] ? artist : "--");
    lv_label_set_text(g_spec_song_lbl, title[0] ? title : "--");

    /* 歌词: 命中缓存直接重建; 否则清空显示"加载中", SD读取+解析放后台任务
     * (旧版在 LVGL 线程同步读卡+解析+建 300 个 label, 切歌会卡 UI 数百 ms) */
    if (index >= 0 && index == s_lyric_loaded_index) {
        rebuild_lyrics();
        update_lyric_highlight(0);
    } else {
        g_lyric_count = 0;
        g_lyric_active = -1;
        show_lyric_hint("歌词加载中...");
        if (index >= 0) {
            lyric_request_load(index);
        }
    }

    /* 收藏状态 */
    update_fav_btn();
}

/* ========================================================================
 * 播放状态显示 (含 BUFFERING 等待指示)
 * ======================================================================== */

/* 应用播放状态到控件: BUFFERING 显示 spinner 隐藏图标,
 * PLAYING 显示暂停图标, 其余显示播放图标 */
static void apply_audio_state(audio_state_t st)
{
    g_state = st;
    bool buffering = (st == AUDIO_STATE_BUFFERING);

    if (g_play_spinner) {
        if (buffering) {
            lv_obj_clear_flag(g_play_spinner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_play_spinner, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (g_play_icon) {
        if (buffering) {
            lv_obj_add_flag(g_play_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(g_play_icon, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(g_play_icon,
                              (st == AUDIO_STATE_PLAYING) ? LV_SYMBOL_PAUSE
                                                          : LV_SYMBOL_PLAY);
        }
    }
}

/* 状态回调 (audio_player 播放任务线程) -> lv_async_call 转到 LVGL 线程 */
static void state_async_cb(void *p)
{
    apply_audio_state((audio_state_t)(intptr_t)p);
}

static void ui_state_cb(audio_state_t state)
{
    ui_lvgl_async_call(state_async_cb, (void *)(intptr_t)state);
}

/* ========================================================================
 * 时间定时器 (1s: 状态栏时钟 + 睡眠剩余; 80ms: 唱片旋转 + 频谱)
 * ======================================================================== */

static lv_timer_t *g_clock_timer = NULL;
static lv_timer_t *g_anim_timer = NULL;

static void cb_timer(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm *ti = localtime(&now);
    if (ti) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", ti->tm_hour, ti->tm_min);
        lv_label_set_text(g_time_lbl, buf);
    }

    /* 睡眠剩余显示 */
    uint32_t remain = audio_player_get_sleep_remaining();
    if (g_sleep_lbl) {
        if (remain > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%lu分", (unsigned long)(remain / 60));
            lv_label_set_text(g_sleep_lbl, buf);
        } else {
            lv_label_set_text(g_sleep_lbl, "睡眠");
        }
    }
}

static void cb_anim_timer(lv_timer_t *t)
{
    (void)t;

    if (g_tab == TAB_PLAY && g_state == AUDIO_STATE_PLAYING) {
        g_vinyl_angle += 5;
        if (g_vinyl_angle >= 360) g_vinyl_angle -= 360;
        vinyl_draw(g_vinyl_angle);
    }

    if (g_tab == TAB_SPEC) {
        update_spec_bars();
    }
}

/* ========================================================================
 * 公开 API
 * ======================================================================== */

static lv_obj_t *g_player_scr = NULL;

static void cb_back_to_desktop(lv_event_t *e)
{
    (void)e;
    extern void ui_desktop_show(void);
    ui_desktop_show();
}

static void swipe_back_to_desktop(void)
{
    ui_navbar_back();
}

void ui_music_player_init(void)
{
    if (g_player_scr) return;

    g_player_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_player_scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(g_player_scr, LV_OPA_COVER, 0);

    ESP_LOGI("MUSIC", "init: status bar");
    create_status_bar(g_player_scr);
    ESP_LOGI("MUSIC", "init: tab bar");
    create_tab_bar(g_player_scr);
    ESP_LOGI("MUSIC", "init: page play");
    create_page_play(g_player_scr);
    ESP_LOGI("MUSIC", "init: page lyric");
    create_page_lyric(g_player_scr);
    {
        lv_mem_monitor_t mm;
        lv_mem_monitor(&mm);
        ESP_LOGI("MUSIC", "init: before page spec, lvgl free=%d", (int)mm.free_size);
    }
    ESP_LOGI("MUSIC", "init: page spec");
    create_page_spec(g_player_scr);
    ESP_LOGI("MUSIC", "init: spec page done");

    /* 状态栏返回按钮 */
    lv_obj_t *back_btn = lv_btn_create(g_player_scr);
    lv_obj_set_size(back_btn, 30, 30);
    lv_obj_set_style_radius(back_btn, 15, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_pos(back_btn, 2, 5);
    lv_obj_add_event_cb(back_btn, cb_back_to_desktop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(back_lbl, COLOR_WHITE, 0);
    lv_obj_center(back_lbl);
    ESP_LOGI("MUSIC", "init: back_btn done");

    /* 侧滑返回手势 */
    ui_edge_swipe_add(g_player_scr, swipe_back_to_desktop);
    ESP_LOGI("MUSIC", "init: swipe done");

    /* 默认页签 */
    set_page(TAB_PLAY);
    ESP_LOGI("MUSIC", "init: set_page done");

    /* 定时器 */
    g_clock_timer = lv_timer_create(cb_timer, 1000, NULL);
    ESP_LOGI("MUSIC", "init: clock timer done");
    g_anim_timer = lv_timer_create(cb_anim_timer, 80, NULL);
    ESP_LOGI("MUSIC", "init: anim timer done");

    /* 注册回调 */
    audio_player_set_progress_cb(ui_progress_cb);
    audio_player_set_track_cb(ui_track_cb);
    audio_player_set_state_cb(ui_state_cb);
    ESP_LOGI("MUSIC", "init: callbacks done");

    /* 初始化频谱 */
    ESP_LOGI("MUSIC", "fft_spectrum_init begin");
    fft_spectrum_init(SPEC_BARS);
    ESP_LOGI("MUSIC", "fft_spectrum_init done");

    /* 初始状态 */
    apply_audio_state(audio_player_get_state());

    /* 滑块初始值 */
    g_slider_updating = true;
    lv_slider_set_value(g_vol_slider, bsp_audio_get_volume(), LV_ANIM_OFF);
    g_slider_updating = false;

    update_mode_btn();

    /* 若播放器已在播放 (记忆恢复), 立即刷新信息 */
    int cur = audio_player_get_current_index();
    if (cur >= 0) refresh_track(cur);
}

void ui_music_player_show(void)
{
    if (g_player_scr == NULL) {
        ui_music_player_init();
    }
    ui_navbar_push_screen_named(g_player_scr, false, "音乐");

    /* 每次进入刷新当前信息与状态 (含等待指示) */
    int cur = audio_player_get_current_index();
    if (cur >= 0) {
        refresh_track(cur);
    }
    apply_audio_state(audio_player_get_state());
}

void ui_music_player_update_state(audio_state_t state)
{
    apply_audio_state(state);
}

void ui_music_player_update_progress(uint32_t current_sec, uint32_t total_sec)
{
    g_total_sec = total_sec;
    if (g_seeking) return;
    char buf[16];
    if (total_sec > 0) {
        int32_t pct = (int32_t)((current_sec * 100UL) / total_sec);
        if (pct > 100) pct = 100;
        lv_bar_set_value(g_bar, pct, LV_ANIM_OFF);
    } else {
        lv_bar_set_value(g_bar, 0, LV_ANIM_OFF);
    }
    fmt_time(current_sec, buf, sizeof(buf));
    lv_label_set_text(g_cur_time_lbl, buf);
    fmt_time(total_sec, buf, sizeof(buf));
    lv_label_set_text(g_total_time_lbl, buf);
}

void ui_music_player_update_song(const char *title, const char *artist)
{
    if (title)  lv_label_set_text(g_title_lbl, title);
    if (artist) lv_label_set_text(g_artist_lbl, artist);
}

void ui_music_player_update_playlist(const char **names, int count, int current_index)
{
    g_play_index = current_index;
    /* 首次调用时在 PSRAM 分配 8KB 名称缓冲 (PSRAM 优先, 失败回退内部 RAM) */
    if (g_names == NULL) {
        g_names = (char (*)[128])bsp_memory_malloc(PLAYLIST_MAX * 128);
    }
    if (g_names == NULL) {
        g_names_count = 0;
        return;
    }
    g_names_count = (count > PLAYLIST_MAX) ? PLAYLIST_MAX : count;
    for (int i = 0; i < g_names_count && names && names[i]; i++) {
        size_t n = strnlen(names[i], sizeof(g_names[i]) - 1);
        memcpy(g_names[i], names[i], n);
        g_names[i][n] = '\0';
    }
}

void ui_music_player_update_wifi(bool connected, const char *ip)
{
    (void)ip;
    lv_obj_set_style_text_color(g_wifi_lbl, connected ? COLOR_WHITE : COLOR_GRAY, 0);
}

void ui_music_player_update_bt(bool connected)
{
    lv_obj_set_style_text_color(g_bt_lbl, connected ? COLOR_WHITE : COLOR_GRAY, 0);
}

/* 播放器内已不再放置亮度滑块 (亮度调节在系统设置页), 保留接口兼容 */
void ui_music_player_set_brightness_slider(uint8_t value)
{
    (void)value;
}

void ui_music_player_set_volume_slider(uint8_t value)
{
    if (!g_vol_slider) return;
    g_slider_updating = true;
    lv_slider_set_value(g_vol_slider, value, LV_ANIM_ON);
    g_slider_updating = false;
}
