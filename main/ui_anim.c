/*******************************************************************************
 * 通用 UI 动画工具实现 - LVGL 9.x (ESP-IDF C)
 *
 * 实现说明:
 *   - 所有动画通过 lv_anim 播放, 由 LVGL 内部定时器驱动, 无阻塞延时;
 *   - exec 回调直接写样式属性 (transform_scale / translate / opa),
 *     触发 LVGL 局部重绘;
 *   - 入场动画使用并行 lv_anim (各属性同时播放), 无需创建/释放时间轴,
 *     轻量且不存在时间轴生命周期管理问题;
 *   - 页面切换 / 弹窗等复合联动动画在 ui_page / ui_dialog 中基于
 *     lv_anim_timeline 实现。
 *
 * 性能调优要点:
 *   - 缩放/位移动画是纯样式变换, SW render 下开销与动画区域面积成正比,
 *     卡片/弹窗这类小区域动画开销很小;
 *   - 缓动曲线: overshoot 会越过目标值产生"回弹"手感, bounce 用于落地效果;
 *   - duration 参考: 按压 60ms, 回弹 240ms, 卡片入场 300ms, 弹窗 250ms。
 ******************************************************************************/

#include "ui_anim.h"
#include <stdlib.h>

/* ========================================================================
 * 样式属性 exec 包装 (两参版本, 供 lv_anim exec_cb 使用)
 * ======================================================================== */

void ui_anim_set_style_translate_x(void *var, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, v, 0);
}

void ui_anim_set_style_translate_y(void *var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, v, 0);
}

void ui_anim_set_style_scale_xy(void *var, int32_t v)
{
    /* LV_SCALE_NONE(256) = 100%, 两轴同步缩放保持等比 */
    lv_obj_set_style_transform_scale_x((lv_obj_t *)var, v, 0);
    lv_obj_set_style_transform_scale_y((lv_obj_t *)var, v, 0);
}

void ui_anim_set_style_opa(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/* ========================================================================
 * 缓动曲线映射
 * ======================================================================== */

lv_anim_path_cb_t ui_anim_get_path(ui_anim_ease_t ease)
{
    switch (ease) {
        case UI_ANIM_EASE_LINEAR:    return lv_anim_path_linear;
        case UI_ANIM_EASE_IN:        return lv_anim_path_ease_in;
        case UI_ANIM_EASE_OUT:       return lv_anim_path_ease_out;
        case UI_ANIM_EASE_IN_OUT:    return lv_anim_path_ease_in_out;
        case UI_ANIM_EASE_OVERSHOOT: return lv_anim_path_overshoot;
        case UI_ANIM_EASE_BOUNCE:    return lv_anim_path_bounce;
        default:                     return lv_anim_path_ease_out;
    }
}

/* ========================================================================
 * 基础动画播放
 * ======================================================================== */

void ui_anim_play(lv_obj_t *obj, lv_anim_exec_xcb_t exec_cb,
                  int32_t from, int32_t to,
                  uint32_t duration, uint32_t delay, ui_anim_ease_t ease)
{
    ui_anim_play_cb(obj, exec_cb, from, to, duration, delay, ease, NULL);
}

void ui_anim_play_cb(lv_obj_t *obj, lv_anim_exec_xcb_t exec_cb,
                     int32_t from, int32_t to,
                     uint32_t duration, uint32_t delay, ui_anim_ease_t ease,
                     lv_anim_completed_cb_t done_cb)
{
    if (obj == NULL || exec_cb == NULL) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, ui_anim_get_path(ease));
    if (done_cb) lv_anim_set_completed_cb(&a, done_cb);
    lv_anim_start(&a);
}

/* ---- 单一属性便捷封装 ---- */

void ui_anim_fade(lv_obj_t *obj, lv_opa_t from, lv_opa_t to,
                  uint32_t dur, uint32_t delay, ui_anim_ease_t ease)
{
    ui_anim_play(obj, ui_anim_set_style_opa, from, to, dur, delay, ease);
}

void ui_anim_move_x(lv_obj_t *obj, int32_t from, int32_t to,
                    uint32_t dur, uint32_t delay, ui_anim_ease_t ease)
{
    ui_anim_play(obj, ui_anim_set_style_translate_x, from, to, dur, delay, ease);
}

void ui_anim_move_y(lv_obj_t *obj, int32_t from, int32_t to,
                    uint32_t dur, uint32_t delay, ui_anim_ease_t ease)
{
    ui_anim_play(obj, ui_anim_set_style_translate_y, from, to, dur, delay, ease);
}

void ui_anim_scale(lv_obj_t *obj, int32_t from, int32_t to,
                   uint32_t dur, uint32_t delay, ui_anim_ease_t ease)
{
    ui_anim_play(obj, ui_anim_set_style_scale_xy, from, to, dur, delay, ease);
}

void ui_anim_stop_all(lv_obj_t *obj)
{
    if (obj == NULL) return;
    /* 第二个参数 NULL 表示删除该对象上的所有动画 */
    lv_anim_delete(obj, NULL);
}

/* ========================================================================
 * 入场动画
 *
 * 实现: 各属性动画并行播放 (同一 delay 起步), 互不依赖, 无需时间轴;
 *       动画结束后 lv_anim 自动释放, 无内存泄漏。
 * ======================================================================== */

/* 把对象 transform 的 pivot 设为中心点 (缩放围绕中心进行) */
static void set_pivot_center(lv_obj_t *obj)
{
    lv_obj_set_style_transform_pivot_x(obj, lv_obj_get_width(obj) / 2, 0);
    lv_obj_set_style_transform_pivot_y(obj, lv_obj_get_height(obj) / 2, 0);
}

void ui_anim_entrance(lv_obj_t *obj, ui_anim_entrance_t type,
                      uint32_t duration, uint32_t delay)
{
    if (obj == NULL) return;

    switch (type) {
        case UI_ANIM_ENTRANCE_FADE_UP:
            /* 淡入 + 上移 24px, ease_out 先快后慢 */
            ui_anim_move_y(obj, 24, 0, duration, delay, UI_ANIM_EASE_OUT);
            ui_anim_fade(obj, 0, LV_OPA_COVER, duration, delay, UI_ANIM_EASE_OUT);
            break;

        case UI_ANIM_ENTRANCE_ZOOM:
            /* 中心缩放弹出: 0.7x -> 1.0x, overshoot 产生回弹手感 */
            set_pivot_center(obj);
            ui_anim_scale(obj, (int32_t)(7 * LV_SCALE_NONE / 10), LV_SCALE_NONE,
                          duration, delay, UI_ANIM_EASE_OVERSHOOT);
            break;

        case UI_ANIM_ENTRANCE_SLIDE_LEFT:
            /* 自左侧滑入 48px + 淡入 */
            ui_anim_move_x(obj, -48, 0, duration, delay, UI_ANIM_EASE_OUT);
            ui_anim_fade(obj, 0, LV_OPA_COVER, duration, delay, UI_ANIM_EASE_OUT);
            break;

        case UI_ANIM_ENTRANCE_SLIDE_RIGHT:
            /* 自右侧滑入 48px + 淡入 */
            ui_anim_move_x(obj, 48, 0, duration, delay, UI_ANIM_EASE_OUT);
            ui_anim_fade(obj, 0, LV_OPA_COVER, duration, delay, UI_ANIM_EASE_OUT);
            break;

        case UI_ANIM_ENTRANCE_ZOOM_FADE:
            /* 缩放(overshoot) + 淡入(ease_out) 复合 */
            set_pivot_center(obj);
            ui_anim_scale(obj, (int32_t)(7 * LV_SCALE_NONE / 10), LV_SCALE_NONE,
                          duration, delay, UI_ANIM_EASE_OVERSHOOT);
            ui_anim_fade(obj, 0, LV_OPA_COVER, duration, delay, UI_ANIM_EASE_OUT);
            break;

        default:
            break;
    }
}

void ui_anim_entrance_stagger(lv_obj_t **objs, uint32_t cnt,
                              ui_anim_entrance_t type,
                              uint32_t duration, uint32_t stagger_ms)
{
    if (objs == NULL || cnt == 0) return;

    for (uint32_t i = 0; i < cnt; i++) {
        if (objs[i] == NULL) continue;
        /* 错峰延迟: 后一个卡比前一个晚 stagger_ms 起步, 形成波浪式入场 */
        ui_anim_entrance(objs[i], type, duration, i * stagger_ms);
    }
}

/* ========================================================================
 * 按压回弹
 *
 * 事件选择说明 (LVGL 9.3):
 *   - LV_EVENT_PRESSED:    按下
 *   - LV_EVENT_RELEASED:   任何情况下释放都会发送 (含滑出对象后释放)
 *   - LV_EVENT_PRESS_LOST:  按下后手指滑出对象 (仍未释放) -> 提前恢复
 *   - LV_EVENT_CLICKED:     未滚动的有效点击 -> 确保恢复
 * 注意: LVGL 9.3 没有 LV_EVENT_CANCELED / RELEASED_AFTER_SCROLL,
 *       所有释放场景由 RELEASED 覆盖。
 * ======================================================================== */

#define PRESS_SPRING_DOWN_SCALE ((int32_t)(9 * LV_SCALE_NONE / 10)) /* 按下: 0.9x */

static void press_spring_event_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    switch (code) {
        case LV_EVENT_PRESSED:
            /* 按下: 快速缩小到 0.9x, ease_in 先慢后快 (60ms) */
            ui_anim_scale(obj, LV_SCALE_NONE, PRESS_SPRING_DOWN_SCALE,
                          60, 0, UI_ANIM_EASE_IN);
            break;

        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
        case LV_EVENT_CLICKED:
            /* 释放: overshoot 回弹到 1.0x (240ms 手感最佳) */
            ui_anim_scale(obj, PRESS_SPRING_DOWN_SCALE, LV_SCALE_NONE,
                          240, 0, UI_ANIM_EASE_OVERSHOOT);
            break;

        default:
            break;
    }
}

void ui_anim_attach_press_spring(lv_obj_t *obj)
{
    if (obj == NULL) return;

    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    /* 缩放围绕中心 */
    set_pivot_center(obj);

    lv_obj_add_event_cb(obj, press_spring_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, press_spring_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(obj, press_spring_event_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(obj, press_spring_event_cb, LV_EVENT_CLICKED, NULL);
}
