/*******************************************************************************
 * 通用 UI 动画工具 - LVGL 9.x (ESP-IDF C)
 *
 * 屏幕: 262x928 竖屏, MIPI-DSI, 软件渲染 (SW render)
 *
 * 功能:
 *   - 任意 lv_obj_t 的入场动画 (淡入上移 / 缩放弹出 / 左右滑入 / 缩放+淡入)
 *   - 多卡片级联入场 (时间轴错峰)
 *   - 按钮/卡片按压回弹动画
 *   - 单一属性动画便捷封装 (translate / scale / opa)
 *
 * 设计要点 (性能):
 *   - 全部基于 lv_anim / lv_anim_timeline, 由 LVGL 定时器驱动, 无阻塞延时;
 *   - 动画只改样式属性 (transform_scale / translate / opa), 触发局部重绘,
 *     不会引起整屏红raw;
 *   - 动画结束后自动释放, 不持有对象引用;
 *   - 回调内不做耗时运算。
 *
 * 本文件只使用 LVGL9 新 API (lv_anim_*, lv_anim_timeline_*, lv_obj_set_style_*),
 * 不使用任何 LVGL8 废弃 API。
 ******************************************************************************/
#ifndef UI_ANIM_H_INCLUDED
#define UI_ANIM_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

/* ========================================================================
 * 缓动曲线枚举 (映射到 LVGL9 的 lv_anim_path_*)
 * ======================================================================== */
typedef enum {
    UI_ANIM_EASE_LINEAR = 0,   /* lv_anim_path_linear     匀速 */
    UI_ANIM_EASE_IN,           /* lv_anim_path_ease_in    先慢后快 */
    UI_ANIM_EASE_OUT,          /* lv_anim_path_ease_out   先快后慢 (入场常用) */
    UI_ANIM_EASE_IN_OUT,       /* lv_anim_path_ease_in_out 两头慢中间快 */
    UI_ANIM_EASE_OVERSHOOT,    /* lv_anim_path_overshoot  回弹超调 (弹窗/卡片弹出用) */
    UI_ANIM_EASE_BOUNCE,       /* lv_anim_path_bounce     落地弹跳 */
} ui_anim_ease_t;

/* ========================================================================
 * 入场动画类型
 * ======================================================================== */
typedef enum {
    UI_ANIM_ENTRANCE_FADE_UP = 0,  /* 淡入 + 上移 24px (列表卡片常用) */
    UI_ANIM_ENTRANCE_ZOOM,         /* 中心缩放弹出 (overshoot 回弹) */
    UI_ANIM_ENTRANCE_SLIDE_LEFT,   /* 自左侧滑入 */
    UI_ANIM_ENTRANCE_SLIDE_RIGHT,  /* 自右侧滑入 */
    UI_ANIM_ENTRANCE_ZOOM_FADE,    /* 缩放 + 淡入 复合 */
} ui_anim_entrance_t;

/* ========================================================================
 * 样式属性 exec 包装函数 (可直接作为 lv_anim_set_exec_cb 的参数)
 *
 * 说明: LVGL9 的 lv_obj_set_style_* 需要 3 个参数 (obj, value, selector),
 *       而 lv_anim 的 exec_cb 只有 (var, value) 两个参数,
 *       所以这里提供两参包装。全部写入 selector=0 (默认态)。
 * ======================================================================== */
void ui_anim_set_style_translate_x(void *var, int32_t v);
void ui_anim_set_style_translate_y(void *var, int32_t v);
void ui_anim_set_style_scale_xy(void *var, int32_t v);   /* 同时缩放 X 和 Y (保持等比) */
void ui_anim_set_style_opa(void *var, int32_t v);

/* ========================================================================
 * 基础 API
 * ======================================================================== */

/**
 * @brief 获取 LVGL9 缓动路径回调
 * @param ease 缓动类型
 * @return lv_anim_path_cb_t (lv_anim_set_path_cb 的参数)
 */
lv_anim_path_cb_t ui_anim_get_path(ui_anim_ease_t ease);

/**
 * @brief 播放单个属性动画 (不带完成回调)
 */
void ui_anim_play(lv_obj_t *obj, lv_anim_exec_xcb_t exec_cb,
                  int32_t from, int32_t to,
                  uint32_t duration, uint32_t delay, ui_anim_ease_t ease);

/**
 * @brief 播放单个属性动画 (带完成回调, 用于动画链)
 *
 * @note 若同一 obj + exec_cb 的动画正在运行, 会被本调用覆盖重启
 */
void ui_anim_play_cb(lv_obj_t *obj, lv_anim_exec_xcb_t exec_cb,
                     int32_t from, int32_t to,
                     uint32_t duration, uint32_t delay, ui_anim_ease_t ease,
                     lv_anim_completed_cb_t done_cb);

/* ---- 单一属性便捷封装 ---- */
void ui_anim_fade(lv_obj_t *obj, lv_opa_t from, lv_opa_t to,
                  uint32_t dur, uint32_t delay, ui_anim_ease_t ease);
void ui_anim_move_x(lv_obj_t *obj, int32_t from, int32_t to,
                    uint32_t dur, uint32_t delay, ui_anim_ease_t ease);
void ui_anim_move_y(lv_obj_t *obj, int32_t from, int32_t to,
                    uint32_t dur, uint32_t delay, ui_anim_ease_t ease);
void ui_anim_scale(lv_obj_t *obj, int32_t from, int32_t to,
                   uint32_t dur, uint32_t delay, ui_anim_ease_t ease);

/**
 * @brief 停止对象上所有动画
 *
 * 说明: lv_anim_delete(obj, NULL) 删除所有绑定到该对象的动画。
 */
void ui_anim_stop_all(lv_obj_t *obj);

/* ========================================================================
 * 入场动画
 * ======================================================================== */

/**
 * @brief 给任意对象播放入场动画
 *
 * 内部使用并行 lv_anim 同时驱动多个样式属性 (translate/scale/opa),
 * 自动把对象 pivot 设置为中心点, 动画结束后由 lv_anim 自动释放资源。
 * 适合任意卡片/列表项; 复合时间轴动画见 ui_dialog / ui_page。
 *
 * @param obj       目标对象 (创建好后、显示前调用)
 * @param type      入场动画类型
 * @param duration  动画时长 (ms)
 * @param delay     延迟 (ms), 用于错峰
 */
void ui_anim_entrance(lv_obj_t *obj, ui_anim_entrance_t type,
                      uint32_t duration, uint32_t delay);

/**
 * @brief 多对象级联入场 (列表卡片依次弹出)
 *
 * @param objs       对象数组 (按显示顺序)
 * @param cnt        对象数量
 * @param type       入场动画类型
 * @param duration   单个对象动画时长 (ms)
 * @param stagger_ms 相邻对象错峰间隔 (ms), 例如 60ms 起步逐卡加速
 */
void ui_anim_entrance_stagger(lv_obj_t **objs, uint32_t cnt,
                              ui_anim_entrance_t type,
                              uint32_t duration, uint32_t stagger_ms);

/* ========================================================================
 * 按压回弹
 * ======================================================================== */

/**
 * @brief 给可点击控件附加按压缩放回弹效果
 *
 * 自动挂 LV_EVENT_PRESSED / LV_EVENT_RELEASED / LV_EVENT_PRESS_LOST
 * 事件回调: 按下快速缩小到 0.9x, 释放用 overshoot 回弹到 1.0x。
 *
 * @note 适用于独立按钮/卡片。若控件位于可滚动列表内, 按下时会同时触发
 *       列表滚动, 回弹效果可能被滚动动画打断; 这种情况请自行按需调用
 *       ui_anim_scale()。
 * @param obj 目标对象 (会自动设置中心点 pivot)
 */
void ui_anim_attach_press_spring(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif

#endif /* UI_ANIM_H_INCLUDED */
