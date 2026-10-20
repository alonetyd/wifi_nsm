/*******************************************************************************
 * 安卓风格三键导航栏 UI 模块实现 - LVGL 9.x
 *
 * 屏幕: 262×928 (竖屏) RGB565
 *
 * 实现:
 *   - 三键导航栏: 返回 / 主页 / 后台 (使用 layer_top 持久浮层)
 *   - 屏幕导航栈: 记录历史屏幕, 支持返回与最近应用
 *   - 最近应用视图: 显示栈中所有屏幕的卡片, 可切换或清除
 *
 * 内存策略:
 *   - 导航栈使用 PSRAM 分配
 *   - 屏幕对象由各模块自行管理生命周期
 *   - 仅记录屏幕指针 + 名称, 不复制内容
 ******************************************************************************/

#include "ui_navbar.h"
#include "ui_desktop.h"
#include "esp_2_41ttf.h"
#include "bsp_theme.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ui_navbar";

/* ======================== 屏幕尺寸 / 布局 ======================== */
#define SCREEN_W   262
#define SCREEN_H   928
#define NAVBAR_H   UI_NAVBAR_H              /* 导航栏高度 */
#define RECENTS_CARD_W  (SCREEN_W - 24)
#define RECENTS_CARD_H  180
#define RECENTS_GAP     8

/* ======================== 导航栈条目 ======================== */
typedef struct {
    lv_obj_t *screen;                      /* 屏幕对象 */
    char      name[32];                    /* 页面名称 (用于最近应用列表显示) */
} nav_entry_t;

#define NAV_STACK_MAX  8
static nav_entry_t *s_stack = NULL;         /* 在 PSRAM 中分配 */
static int          s_stack_top = -1;       /* 栈顶索引, -1 表示空栈 */

/* ======================== 最近应用列表 (独立于导航栈) ========================
 * 与导航栈分离: 按主页键清空导航栈时, 最近应用仍然保留,
 * 这样后台键才能看到之前打开过的应用 (安卓式行为). */
static nav_entry_t s_recents[NAV_STACK_MAX];
static int         s_recents_top = -1;      /* -1 表示无最近应用 */

/* ======================== UI 对象 ======================== */
static lv_obj_t *g_navbar = NULL;            /* 导航栏容器 (layer_top) */
static lv_obj_t *g_btn_back = NULL;
static lv_obj_t *g_btn_home = NULL;
static lv_obj_t *g_btn_recents = NULL;
static bool      g_visible = true;

/* 最近应用列表 */
static lv_obj_t *g_recents_mask = NULL;      /* 半透明遮罩 */
static lv_obj_t *g_recents_panel = NULL;     /* 滚动列表容器 */
static bool      g_recents_visible = false;

/* ======================== 颜色定义 (使用主题系统) ======================== */
static inline lv_color_t theme_bg(void)      { return bsp_theme_colors()->bg; }
static inline lv_color_t theme_card(void)    { return bsp_theme_colors()->card; }
static inline lv_color_t theme_primary(void) { return bsp_theme_colors()->primary; }
static inline lv_color_t theme_accent(void) { return bsp_theme_colors()->accent; }

#define COLOR_BG        theme_bg()
#define COLOR_CARD      theme_card()
#define COLOR_PRIMARY   theme_primary()
#define COLOR_ACCENT    theme_accent()
#define COLOR_WHITE     lv_color_hex(0xffffff)
#define COLOR_GRAY      lv_color_hex(0xa0a0a0)
#define COLOR_TRANSPARENT lv_color_hex(0x000000)

/* ======================== 前置声明 ======================== */
static void cb_back_btn(lv_event_t *e);
static void cb_home_btn(lv_event_t *e);
static void cb_recents_btn(lv_event_t *e);
static void cb_recents_mask_click(lv_event_t *e);
static void cb_recents_card_click(lv_event_t *e);
static void cb_recents_clear_btn(lv_event_t *e);
static void cb_navbar_anim_done(lv_anim_t *a);

static lv_obj_t *create_nav_button(lv_obj_t *parent, const char *icon,
                                   lv_event_cb_t cb);
static void build_recents_list(void);
static void clear_recents_list(void);
static void load_screen_internal(lv_obj_t *scr, bool anim, bool left);
void ui_navbar_push_screen_named(lv_obj_t *new_scr, bool anim, const char *name);

/* ========================================================================
 * 内部辅助: 栈操作
 * ======================================================================== */

static bool stack_init(void)
{
    if (s_stack == NULL) {
        s_stack = (nav_entry_t *)heap_caps_malloc(
            sizeof(nav_entry_t) * NAV_STACK_MAX, MALLOC_CAP_SPIRAM);
        if (s_stack == NULL) {
            ESP_LOGW(TAG, "PSRAM不可用, 栈使用普通内存");
            s_stack = (nav_entry_t *)malloc(sizeof(nav_entry_t) * NAV_STACK_MAX);
        }
        if (s_stack == NULL) {
            ESP_LOGE(TAG, "导航栈内存分配失败");
            return false;
        }
        memset(s_stack, 0, sizeof(nav_entry_t) * NAV_STACK_MAX);
        s_stack_top = -1;
    }
    return true;
}

static bool stack_push(lv_obj_t *scr, const char *name)
{
    if (!stack_init()) return false;
    if (s_stack_top >= NAV_STACK_MAX - 1) {
        ESP_LOGW(TAG, "导航栈已满 (max=%d)", NAV_STACK_MAX);
        /* 移除最旧条目, 腾出空间 */
        memmove(&s_stack[0], &s_stack[1],
                sizeof(nav_entry_t) * (NAV_STACK_MAX - 1));
        s_stack_top = NAV_STACK_MAX - 2;
    }
    s_stack_top++;
    s_stack[s_stack_top].screen = scr;
    if (name) {
        strncpy(s_stack[s_stack_top].name, name,
                sizeof(s_stack[s_stack_top].name) - 1);
        s_stack[s_stack_top].name[sizeof(s_stack[s_stack_top].name) - 1] = '\0';
    } else {
        s_stack[s_stack_top].name[0] = '\0';
    }
    return true;
}

static nav_entry_t *stack_pop(void)
{
    if (!s_stack || s_stack_top < 0) return NULL;
    nav_entry_t *e = &s_stack[s_stack_top];
    s_stack_top--;
    return e;
}

static nav_entry_t *stack_top(void)
{
    if (!s_stack || s_stack_top < 0) return NULL;
    return &s_stack[s_stack_top];
}

static bool stack_contains(lv_obj_t *screen)
{
    if (!s_stack || !screen) return false;
    for (int i = 0; i <= s_stack_top; i++) {
        if (s_stack[i].screen == screen) return true;
    }
    return false;
}

static void stack_clear(void)
{
    s_stack_top = -1;
    if (s_stack) memset(s_stack, 0, sizeof(nav_entry_t) * NAV_STACK_MAX);
}

/* ======================== 最近应用列表操作 ======================== */

static void recents_clear(void)
{
    memset(s_recents, 0, sizeof(s_recents));
    s_recents_top = -1;
}

static void recents_add(lv_obj_t *scr, const char *name)
{
    if (!scr) return;

    /* 如果屏幕已存在, 先移除旧条目 (移动到最前) */
    for (int i = 0; i <= s_recents_top; i++) {
        if (s_recents[i].screen == scr) {
            memmove(&s_recents[i], &s_recents[i + 1],
                    sizeof(nav_entry_t) * (s_recents_top - i));
            s_recents_top--;
            break;
        }
    }

    /* 超出容量时丢弃最旧的 */
    if (s_recents_top >= NAV_STACK_MAX - 1) {
        memmove(&s_recents[0], &s_recents[1],
                sizeof(nav_entry_t) * (NAV_STACK_MAX - 1));
        s_recents_top = NAV_STACK_MAX - 2;
    }

    s_recents_top++;
    s_recents[s_recents_top].screen = scr;
    memset(s_recents[s_recents_top].name, 0, sizeof(s_recents[s_recents_top].name));
    if (name) {
        strncpy(s_recents[s_recents_top].name, name,
                sizeof(s_recents[s_recents_top].name) - 1);
        s_recents[s_recents_top].name[sizeof(s_recents[s_recents_top].name) - 1] = '\0';
    }
}

/* ========================================================================
 * 内部辅助: 屏幕加载
 * ======================================================================== */

static void load_screen_internal(lv_obj_t *scr, bool anim, bool left)
{
    if (scr == NULL) {
        ESP_LOGW(TAG, "load_screen_internal: scr=NULL");
        return;
    }
    if (anim) {
        lv_screen_load_anim(scr,
            left ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
            300, 0, false);
    } else {
        lv_screen_load(scr);
    }
}

/* ========================================================================
 * 按钮创建
 * ======================================================================== */

static lv_obj_t *create_nav_button(lv_obj_t *parent, const char *icon,
                                   lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, SCREEN_W / 3 - 4, NAVBAR_H - 8);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(btn, COLOR_PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, icon);
    lv_obj_set_style_text_font(lbl, &esp_2, 0);
    lv_obj_set_style_text_color(lbl, COLOR_WHITE, 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* ========================================================================
 * 导航栏事件回调
 * ======================================================================== */

static void cb_back_btn(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "返回键");
    /* 如果最近应用列表可见, 先关闭它 */
    if (g_recents_visible) {
        ui_navbar_hide_recents();
        return;
    }
    ui_navbar_back();
}

static void cb_home_btn(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "主页键");
    if (g_recents_visible) {
        ui_navbar_hide_recents();
    }
    ui_navbar_home();
}

static void cb_recents_btn(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "后台键");
    if (g_recents_visible) {
        ui_navbar_hide_recents();
    } else {
        ui_navbar_show_recents();
    }
}

static void cb_recents_mask_click(lv_event_t *e)
{
    (void)e;
    ui_navbar_hide_recents();
}

static void cb_recents_card_click(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_target(e);
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    (void)card;

    if (idx < 0 || idx > s_recents_top) {
        ui_navbar_hide_recents();
        return;
    }

    /* 用户点击某个最近应用卡片 - 切换到该应用 */
    nav_entry_t *entry = &s_recents[idx];
    lv_obj_t *target = entry->screen;
    char name[32];
    memset(name, 0, sizeof(name));
    if (target != NULL) {
        strncpy(name, entry->name, sizeof(name) - 1);
    }
    if (target == NULL) {
        ui_navbar_hide_recents();
        return;
    }

    /* 把该应用移到最近应用最前 (注意: 会移动 s_recents, 需先拷贝名称) */
    recents_add(target, name);
    /* 从最近应用切换时, 清空返回栈, 返回键将回到桌面 */
    stack_clear();
    ui_navbar_hide_recents();
    load_screen_internal(target, true, true);
    ESP_LOGI(TAG, "切换到最近应用: %s", name);
}

static void cb_recents_clear_btn(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "清除所有最近应用");
    /* 清空导航栈与最近应用记录 (屏幕对象由各模块管理, 这里仅清记录) */
    stack_clear();
    recents_clear();
    ui_navbar_hide_recents();
}

static void cb_navbar_anim_done(lv_anim_t *a)
{
    (void)a;
    /* 动画完成回调 (留空) */
}

/* ========================================================================
 * 最近应用列表构建
 * ======================================================================== */

static void clear_recents_list(void)
{
    if (g_recents_panel == NULL) return;
    /* 删除所有子对象 */
    lv_obj_clean(g_recents_panel);
}

static void build_recents_list(void)
{
    if (g_recents_panel == NULL) return;
    clear_recents_list();

    if (s_recents_top < 0) {
        /* 无最近应用: 显示提示 */
        lv_obj_t *hint = lv_label_create(g_recents_panel);
        lv_label_set_text(hint, "没有最近应用");
        lv_obj_set_style_text_font(hint, &esp_2, 0);
        lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
        lv_obj_center(hint);
        return;
    }

    /* 从最近应用列表末尾到开头依次显示卡片 (最近使用的在最上) */
    for (int i = s_recents_top; i >= 0; i--) {
        nav_entry_t *entry = &s_recents[i];
        if (entry->screen == NULL) continue;

        /* 卡片容器 */
        lv_obj_t *card = lv_obj_create(g_recents_panel);
        lv_obj_set_size(card, RECENTS_CARD_W, RECENTS_CARD_H);
        lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(card, COLOR_PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(card, LV_OPA_40, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(card, COLOR_ACCENT, LV_STATE_FOCUSED);

        /* 页面名称 */
        lv_obj_t *name_lbl = lv_label_create(card);
        const char *display = entry->name[0] ? entry->name : "未命名应用";
        lv_label_set_text(name_lbl, display);
        lv_obj_set_style_text_font(name_lbl, &esp_2, 0);
        lv_obj_set_style_text_color(name_lbl, COLOR_WHITE, 0);
        lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 4, 4);

        /* 屏幕预览区域 (使用截图或简化占位) */
        lv_obj_t *preview = lv_obj_create(card);
        lv_obj_set_size(preview, RECENTS_CARD_W - 16, RECENTS_CARD_H - 50);
        lv_obj_align(preview, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(preview, COLOR_BG, 0);
        lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(preview, 8, 0);
        lv_obj_set_style_border_width(preview, 0, 0);
        lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);

        /* 预览中显示页面名称 (作为占位, 真实截图开销过大) */
        lv_obj_t *prev_lbl = lv_label_create(preview);
        lv_label_set_text(prev_lbl, display);
        lv_obj_set_style_text_font(prev_lbl, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(prev_lbl, COLOR_GRAY, 0);
        lv_obj_center(prev_lbl);

        /* 索引作为 user_data 传递 */
        intptr_t idx = (intptr_t)i;
        lv_obj_add_event_cb(card, cb_recents_card_click, LV_EVENT_CLICKED,
                            (void *)idx);
    }

    /* 底部清除全部按钮 */
    lv_obj_t *clear_btn = lv_btn_create(g_recents_panel);
    lv_obj_set_size(clear_btn, RECENTS_CARD_W, 44);
    lv_obj_set_style_bg_color(clear_btn, COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(clear_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(clear_btn, 8, 0);
    lv_obj_set_style_shadow_width(clear_btn, 0, 0);
    lv_obj_set_style_border_width(clear_btn, 0, 0);
    lv_obj_add_event_cb(clear_btn, cb_recents_clear_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clear_lbl = lv_label_create(clear_btn);
    lv_label_set_text(clear_lbl, "清除全部");
    lv_obj_set_style_text_font(clear_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(clear_lbl, COLOR_WHITE, 0);
    lv_obj_center(clear_lbl);
}

/* ========================================================================
 * 公开 API
 * ======================================================================== */

void ui_navbar_init(void)
{
    if (g_navbar != NULL) return;

    /* 分配导航栈 */
    if (!stack_init()) {
        ESP_LOGE(TAG, "导航栈初始化失败");
        return;
    }

    /* 创建导航栏容器, 挂在 layer_top (在所有屏幕之上) */
    lv_obj_t *layer = lv_layer_top();
    g_navbar = lv_obj_create(layer);
    lv_obj_set_size(g_navbar, SCREEN_W, NAVBAR_H);
    lv_obj_set_pos(g_navbar, 0, SCREEN_H - NAVBAR_H);
    lv_obj_set_style_bg_color(g_navbar, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(g_navbar, LV_OPA_90, 0);
    lv_obj_set_style_radius(g_navbar, 0, 0);
    lv_obj_set_style_border_width(g_navbar, 0, 0);
    lv_obj_set_style_pad_all(g_navbar, 4, 0);
    lv_obj_set_style_pad_column(g_navbar, 4, 0);
    lv_obj_clear_flag(g_navbar, LV_OBJ_FLAG_SCROLLABLE);

    /* 按钮行布局: 三键平均分布 */
    lv_obj_set_flex_flow(g_navbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_navbar, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_btn_back    = create_nav_button(g_navbar, LV_SYMBOL_LEFT,  cb_back_btn);
    g_btn_home    = create_nav_button(g_navbar, LV_SYMBOL_HOME,  cb_home_btn);
    g_btn_recents = create_nav_button(g_navbar, LV_SYMBOL_LIST,  cb_recents_btn);

    /* 确保导航栏在最上层 */
    lv_obj_move_foreground(g_navbar);

    g_visible = true;
    ESP_LOGI(TAG, "导航栏初始化完成 (3键: 返回/主页/后台)");
}

void ui_navbar_show(void)
{
    if (g_navbar == NULL) return;
    if (g_visible) return;

    g_visible = true;
    lv_obj_clear_flag(g_navbar, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_navbar);
    lv_anim_set_values(&a, SCREEN_H, SCREEN_H - NAVBAR_H);
    lv_anim_set_time(&a, 250);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, cb_navbar_anim_done);
    lv_anim_start(&a);
}

void ui_navbar_hide(void)
{
    if (g_navbar == NULL) return;
    if (!g_visible) return;

    g_visible = false;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_navbar);
    lv_anim_set_values(&a, SCREEN_H - NAVBAR_H, SCREEN_H);
    lv_anim_set_time(&a, 250);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, cb_navbar_anim_done);
    lv_anim_start(&a);
}

void ui_navbar_toggle(void)
{
    if (g_visible) ui_navbar_hide();
    else           ui_navbar_show();
}

bool ui_navbar_is_visible(void)
{
    return g_visible;
}

void ui_navbar_push_screen(lv_obj_t *new_scr, bool anim)
{
    ui_navbar_push_screen_named(new_scr, anim, "应用");
}

void ui_navbar_push_screen_named(lv_obj_t *new_scr, bool anim, const char *name)
{
    if (new_scr == NULL) {
        ESP_LOGW(TAG, "push_screen: new_scr=NULL");
        return;
    }
    /* 同一页面重复进入时不重复压栈，避免导航栈出现重复项 */
    lv_obj_t *cur = lv_screen_active();
    if (cur == new_scr) {
        load_screen_internal(new_scr, false, true);
        return;
    }
    if (cur != NULL && !stack_contains(cur)) {
        stack_push(cur, "");
    }
    /* 记录最近应用 (独立于导航栈, 主页键清栈后仍然保留) */
    recents_add(new_scr, name);
    load_screen_internal(new_scr, anim, true);
}

void ui_navbar_replace_top(lv_obj_t *new_scr, bool anim)
{
    if (new_scr == NULL) {
        ESP_LOGW(TAG, "replace_top: new_scr=NULL");
        return;
    }
    /* 替换栈顶屏幕 (不增加栈深度) */
    nav_entry_t *top = stack_top();
    if (top) {
        top->screen = new_scr;
    } else {
        /* 栈为空时退化为 push */
        lv_obj_t *cur = lv_screen_active();
        if (cur != NULL && cur != new_scr) {
            stack_push(cur, "");
        }
    }
    load_screen_internal(new_scr, anim, true);
}

void ui_navbar_back(void)
{
    nav_entry_t *entry = stack_pop();
    if (entry == NULL || entry->screen == NULL) {
        /* 栈为空, 回到桌面 */
        ESP_LOGI(TAG, "导航栈为空, 返回桌面");
        ui_desktop_show();
        return;
    }
    ESP_LOGI(TAG, "返回到: %s",
             entry->name[0] ? entry->name : "(未命名)");
    load_screen_internal(entry->screen, true, false);
}

void ui_navbar_home(void)
{
    /* 清空返回导航栈 (屏幕对象由各模块管理) */
    stack_clear();
    /* 注意: 最近应用记录保留, 后台键仍能看到之前打开的应用 */
    if (g_recents_visible) {
        ui_navbar_hide_recents();
    }
    /* 加载桌面 */
    ui_desktop_show();
    ESP_LOGI(TAG, "返回桌面, 导航栈已清空 (保留 %d 个最近应用)", s_recents_top + 1);
}

void ui_navbar_show_recents(void)
{
    if (g_recents_visible) return;
    if (g_navbar == NULL) {
        ESP_LOGW(TAG, "导航栏未初始化, 无法显示最近应用");
        return;
    }

    /* 创建遮罩 + 列表面板 */
    if (g_recents_mask == NULL) {
        lv_obj_t *layer = lv_layer_top();
        g_recents_mask = lv_obj_create(layer);
        lv_obj_set_size(g_recents_mask, SCREEN_W, SCREEN_H - NAVBAR_H);
        lv_obj_set_pos(g_recents_mask, 0, 0);
        lv_obj_set_style_bg_color(g_recents_mask, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(g_recents_mask, LV_OPA_70, 0);
        lv_obj_set_style_radius(g_recents_mask, 0, 0);
        lv_obj_set_style_border_width(g_recents_mask, 0, 0);
        lv_obj_clear_flag(g_recents_mask, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(g_recents_mask, cb_recents_mask_click,
                            LV_EVENT_CLICKED, NULL);

        /* 滚动列表面板 */
        g_recents_panel = lv_obj_create(g_recents_mask);
        lv_obj_set_size(g_recents_panel, SCREEN_W, SCREEN_H - NAVBAR_H - 16);
        lv_obj_set_pos(g_recents_panel, 0, 8);
        lv_obj_set_style_bg_opa(g_recents_panel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(g_recents_panel, 0, 0);
        lv_obj_set_style_radius(g_recents_panel, 0, 0);
        lv_obj_set_style_pad_all(g_recents_panel, 12, 0);
        lv_obj_set_flex_flow(g_recents_panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(g_recents_panel, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(g_recents_panel, RECENTS_GAP, 0);
        lv_obj_add_flag(g_recents_panel, LV_OBJ_FLAG_SCROLLABLE);
    }

    build_recents_list();

    lv_obj_clear_flag(g_recents_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_recents_mask);
    lv_obj_move_foreground(g_navbar);

    g_recents_visible = true;
    ESP_LOGI(TAG, "显示最近应用列表 (最近应用数=%d)", s_recents_top + 1);
}

void ui_navbar_hide_recents(void)
{
    if (!g_recents_visible) return;
    if (g_recents_mask) {
        lv_obj_add_flag(g_recents_mask, LV_OBJ_FLAG_HIDDEN);
    }
    g_recents_visible = false;
    ESP_LOGI(TAG, "隐藏最近应用列表");
}

bool ui_navbar_recents_is_visible(void)
{
    return g_recents_visible;
}
