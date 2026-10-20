/*******************************************************************************
 * 统一页面切换框架实现 - LVGL 9.x (ESP-IDF C)
 *
 * 实现要点:
 *   - 页面栈: PSRAM 分配, 只存 {screen 指针, name}, 不复制页面内容;
 *   - SLIDE / FADE / OVER 走 lv_screen_load_anim (LVGL 内置动画,
 *     切换期间新旧屏幕同时渲染, 由 LVGL 内部管理, 无撕裂);
 *   - ZOOM 走 lv_anim_timeline: 先预置 scale/opa 样式并立即 lv_screen_load
 *     (避免闪烁), 再播放缩放+淡入时间轴; 时间轴结束后用一次性定时器释放,
 *     避免在时间轴回调内释放自身 (use-after-free);
 *   - 手势: 使用 16ms 周期定时器轮询触摸 indev 状态 (PRESSED/RELEASED)。
 *     为什么不用 LV_EVENT_PRESSED/RELEASED 事件? 因为 LVGL9 的 send_event()
 *     只在命中可点击对象 (indev_obj_act != NULL) 时才会把事件发到 indev,
 *     页面空白区域无控件时手势会失效; 轮询方式与命中无关, 全屏可靠。
 ******************************************************************************/

#include "ui_page.h"
#include "ui_anim.h"   /* 复用样式属性 exec 包装 */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ui_page";

#define UI_PAGE_STACK_MAX  8
#define UI_PAGE_SWIPE_DIST 48   /* 判定为滑动的横向距离阈值 (px) */

/* ========================================================================
 * 页面栈 (PSRAM)
 * ======================================================================== */
typedef struct {
    lv_obj_t *scr;
    char      name[32];
} ui_page_entry_t;

static ui_page_entry_t *s_stack = NULL;
static int s_top = -1;

static bool stack_init(void)
{
    if (s_stack == NULL) {
        s_stack = (ui_page_entry_t *)heap_caps_malloc(
            sizeof(ui_page_entry_t) * UI_PAGE_STACK_MAX, MALLOC_CAP_SPIRAM);
        if (s_stack == NULL) {
            ESP_LOGW(TAG, "PSRAM 不可用, 退回普通内存");
            s_stack = (ui_page_entry_t *)malloc(
                sizeof(ui_page_entry_t) * UI_PAGE_STACK_MAX);
        }
        if (s_stack == NULL) {
            ESP_LOGE(TAG, "页面栈分配失败");
            return false;
        }
        memset(s_stack, 0, sizeof(ui_page_entry_t) * UI_PAGE_STACK_MAX);
        s_top = -1;
    }
    return true;
}

static bool stack_push(lv_obj_t *scr, const char *name)
{
    if (!stack_init()) return false;
    if (s_top >= UI_PAGE_STACK_MAX - 1) {
        ESP_LOGW(TAG, "页面栈已满 (max=%d), 丢弃最旧页面", UI_PAGE_STACK_MAX);
        memmove(&s_stack[0], &s_stack[1],
                sizeof(ui_page_entry_t) * (UI_PAGE_STACK_MAX - 1));
        s_top = UI_PAGE_STACK_MAX - 2;
    }
    s_top++;
    s_stack[s_top].scr = scr;
    if (name) {
        strncpy(s_stack[s_top].name, name, sizeof(s_stack[s_top].name) - 1);
        s_stack[s_top].name[sizeof(s_stack[s_top].name) - 1] = '\0';
    } else {
        s_stack[s_top].name[0] = '\0';
    }
    return true;
}

static ui_page_entry_t *stack_pop(void)
{
    if (s_stack == NULL || s_top < 0) return NULL;
    ui_page_entry_t *e = &s_stack[s_top];
    s_top--;
    return e;
}

/* ========================================================================
 * 过渡动画
 * ======================================================================== */

static void (*s_home_cb)(void) = NULL;

static void page_zoom_cleanup_timer_cb(lv_timer_t *t)
{
    lv_anim_timeline_t *tl = (lv_anim_timeline_t *)lv_timer_get_user_data(t);
    if (tl) lv_anim_timeline_delete(tl); /* 定时器 repeat=1, 自动删除自身 */
}

/* ZOOM 过渡: 缩放 + 淡入 (lv_anim_timeline 时间轴) */
static void page_trans_zoom(lv_obj_t *new_scr, uint32_t time)
{
    if (new_scr == NULL) return;

    lv_display_t *disp = lv_display_get_default();
    lv_coord_t w = lv_display_get_horizontal_resolution(disp);
    lv_coord_t h = lv_display_get_vertical_resolution(disp);

    /* 1) 载入前预置变换样式: 0.8x 缩放 + 透明, 避免闪烁 */
    lv_obj_set_style_transform_pivot_x(new_scr, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(new_scr, h / 2, 0);
    lv_obj_set_style_transform_scale_x(new_scr, (int32_t)(8 * LV_SCALE_NONE / 10), 0);
    lv_obj_set_style_transform_scale_y(new_scr, (int32_t)(8 * LV_SCALE_NONE / 10), 0);
    lv_obj_set_style_opa(new_scr, 0, 0);
    lv_screen_load(new_scr);   /* 立即切换 (无内置动画) */

    /* 2) 时间轴: 缩放回弹 + 淡入 */
    lv_anim_timeline_t *tl = lv_anim_timeline_create();
    if (tl == NULL) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, new_scr);
    lv_anim_set_exec_cb(&a, ui_anim_set_style_scale_xy);
    lv_anim_set_values(&a, (int32_t)(8 * LV_SCALE_NONE / 10), LV_SCALE_NONE);
    lv_anim_set_duration(&a, time);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot); /* 回弹手感 */
    lv_anim_timeline_add(tl, 0, &a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, new_scr);
    lv_anim_set_exec_cb(&a, ui_anim_set_style_opa);
    lv_anim_set_values(&a, 0, LV_OPA_COVER);
    lv_anim_set_duration(&a, time);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_timeline_add(tl, 0, &a);

    lv_anim_timeline_start(tl);

    /* 3) 播放结束后释放时间轴 (延迟执行, 避免在时间轴回调内释放自身) */
    uint32_t playtime = lv_anim_timeline_get_playtime(tl);
    lv_timer_t *t = lv_timer_create(page_zoom_cleanup_timer_cb, playtime + 20, tl);
    lv_timer_set_repeat_count(t, 1);
    LV_UNUSED(t); /* 定时器 repeat=1 会自动删除自身, 句柄无需保存 */
}

/* 统一过渡加载入口 */
static void page_load_with_trans(lv_obj_t *new_scr, ui_page_trans_t trans,
                                 uint32_t time)
{
    if (new_scr == NULL) {
        ESP_LOGW(TAG, "page_load_with_trans: new_scr=NULL");
        return;
    }
    if (time == 0) time = UI_PAGE_TRANS_TIME_DEFAULT;

    switch (trans) {
        case UI_PAGE_TRANS_NONE:
            lv_screen_load(new_scr);
            break;
        case UI_PAGE_TRANS_SLIDE_LEFT:
            /* 新页自右滑入, 旧页向左滑出 */
            lv_screen_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, time, 0, false);
            break;
        case UI_PAGE_TRANS_SLIDE_RIGHT:
            /* 新页自左滑入, 旧页向右滑出 */
            lv_screen_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, time, 0, false);
            break;
        case UI_PAGE_TRANS_FADE:
            lv_screen_load_anim(new_scr, LV_SCR_LOAD_ANIM_FADE_IN, time, 0, false);
            break;
        case UI_PAGE_TRANS_OVER_LEFT:
            lv_screen_load_anim(new_scr, LV_SCR_LOAD_ANIM_OVER_LEFT, time, 0, false);
            break;
        case UI_PAGE_TRANS_OVER_RIGHT:
            lv_screen_load_anim(new_scr, LV_SCR_LOAD_ANIM_OVER_RIGHT, time, 0, false);
            break;
        case UI_PAGE_TRANS_ZOOM:
            page_trans_zoom(new_scr, time);
            break;
        default:
            lv_screen_load(new_scr);
            break;
    }
}

/* ========================================================================
 * 手势滑动翻页 (定时器轮询 indev, 与命中测试无关)
 *
 * 右滑 = 返回上一页; 左滑 = 触发用户注册的"下一页"回调。
 * 判定条件: 横向位移 >= UI_PAGE_SWIPE_DIST 且 |dx| >= 2*|dy|
 * (纵向滚动/点击不会误触发)。
 * ======================================================================== */

static lv_indev_t *s_gesture_indev = NULL;
static lv_timer_t *s_gesture_timer = NULL;
static lv_point_t s_swipe_start;
static bool s_swipe_active = false;
static bool s_gesture_enabled = true;
static ui_page_allow_cb_t s_allow_cb = NULL;
static void *s_allow_ud = NULL;
static ui_page_swipe_cb_t s_swipe_cb = NULL;
static void *s_swipe_ud = NULL;

static void page_gesture_process(lv_point_t *end)
{
    lv_coord_t dx = end->x - s_swipe_start.x;
    lv_coord_t dy = end->y - s_swipe_start.y;

    /* 阈值判定 */
    if (LV_ABS(dx) < UI_PAGE_SWIPE_DIST) return;
    if (LV_ABS(dx) < LV_ABS(dy) * 2) return;

    lv_obj_t *scr = lv_screen_active();

    /* 页面允许回调 (列表页可在此禁用返回手势) */
    if (s_allow_cb && !s_allow_cb(scr, s_allow_ud)) return;

    if (dx > 0) {
        /* 右滑: 返回上一页 */
        if (ui_page_can_back()) {
            ui_page_back(UI_PAGE_TRANS_SLIDE_RIGHT, UI_PAGE_TRANS_TIME_DEFAULT);
        }
    } else {
        /* 左滑: 交给用户回调 (通常用于"下一页") */
        if (s_swipe_cb && s_swipe_cb(scr, UI_PAGE_SWIPE_LEFT, s_swipe_ud)) {
            return;
        }
    }
}

static void page_gesture_poll_cb(lv_timer_t *t)
{
    (void)t;
    lv_indev_t *indev = s_gesture_indev;
    if (indev == NULL || !s_gesture_enabled) return;

    if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
        if (!s_swipe_active) {
            /* 刚按下: 记录起点 */
            lv_indev_get_point(indev, &s_swipe_start);
            s_swipe_active = true;
        }
    } else {
        if (s_swipe_active) {
            /* 刚释放: 判定滑动 */
            s_swipe_active = false;
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            page_gesture_process(&p);
        }
    }
}

/* ========================================================================
 * 初始化
 * ======================================================================== */

void ui_page_init(void)
{
    if (s_gesture_timer != NULL) return; /* 防止重复初始化 */

    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            s_gesture_indev = indev;
            break;
        }
    }
    if (s_gesture_indev == NULL) {
        ESP_LOGW(TAG, "未找到触摸输入设备, 手势翻页不可用");
        return;
    }

    /* 16ms 轮询一次 (约 60fps 节奏), 开销极小; 默认开启手势 */
    s_gesture_timer = lv_timer_create(page_gesture_poll_cb,
                                      LV_DEF_REFR_PERIOD, NULL);
    ESP_LOGI(TAG, "手势滑动翻页已注册 (右滑返回, 左滑下一页)");
}

void ui_page_set_gesture(bool enable)
{
    s_gesture_enabled = enable;
    if (s_gesture_timer == NULL) return;
    if (enable) lv_timer_resume(s_gesture_timer);
    else        lv_timer_pause(s_gesture_timer);
}

void ui_page_set_swipe_allow_cb(ui_page_allow_cb_t cb, void *user_data)
{
    s_allow_cb = cb;
    s_allow_ud = user_data;
}

void ui_page_set_swipe_cb(ui_page_swipe_cb_t cb, void *user_data)
{
    s_swipe_cb = cb;
    s_swipe_ud = user_data;
}

/* ========================================================================
 * 页面栈操作
 * ======================================================================== */

void ui_page_push(lv_obj_t *new_scr, const char *name,
                  ui_page_trans_t trans, uint32_t time)
{
    if (new_scr == NULL) return;

    lv_obj_t *cur = lv_screen_active();
    if (cur != NULL && cur != new_scr) {
        stack_push(cur, name);
    }
    page_load_with_trans(new_scr, trans, time);
    ESP_LOGI(TAG, "push -> %s (depth=%d)", name ? name : "?", s_top + 1);
}

void ui_page_replace(lv_obj_t *new_scr, const char *name,
                     ui_page_trans_t trans, uint32_t time)
{
    if (new_scr == NULL) return;

    if (s_stack && s_top >= 0) {
        /* 替换栈顶屏幕指针 (保持栈深度) */
        s_stack[s_top].scr = new_scr;
        if (name) {
            strncpy(s_stack[s_top].name, name, sizeof(s_stack[s_top].name) - 1);
            s_stack[s_top].name[sizeof(s_stack[s_top].name) - 1] = '\0';
        }
    } else {
        /* 栈为空时退化为 push */
        lv_obj_t *cur = lv_screen_active();
        if (cur != NULL && cur != new_scr) {
            stack_push(cur, name);
        }
    }
    page_load_with_trans(new_scr, trans, time);
}

void ui_page_back(ui_page_trans_t trans, uint32_t time)
{
    ui_page_entry_t *entry = stack_pop();
    if (entry == NULL || entry->scr == NULL) {
        if (s_home_cb) {
            s_home_cb();
        } else {
            ESP_LOGW(TAG, "页面栈为空, 未注册 home 回调, 忽略返回操作");
        }
        return;
    }
    ESP_LOGI(TAG, "back -> %s", entry->name[0] ? entry->name : "?");
    page_load_with_trans(entry->scr, trans, time);
}

void ui_page_set_home_cb(void (*cb)(void))
{
    s_home_cb = cb;
}

bool ui_page_can_back(void)
{
    return (s_stack != NULL) && (s_top >= 0);
}

int ui_page_depth(void)
{
    return s_top + 1;
}

lv_obj_t *ui_page_current(void)
{
    return lv_screen_active();
}

/* ========================================================================
 * 便捷封装
 * ======================================================================== */

void ui_page_push_default(lv_obj_t *new_scr, const char *name)
{
    ui_page_push(new_scr, name, UI_PAGE_TRANS_SLIDE_LEFT, UI_PAGE_TRANS_TIME_DEFAULT);
}

void ui_page_back_default(void)
{
    ui_page_back(UI_PAGE_TRANS_SLIDE_RIGHT, UI_PAGE_TRANS_TIME_DEFAULT);
}
