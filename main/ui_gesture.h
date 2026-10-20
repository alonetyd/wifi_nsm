/**
 * @file ui_gesture.h
 * @brief 侧滑返回手势模块
 *
 * 在屏幕左侧边缘添加不可见的触摸检测区域,
 * 当用户从左边缘向右滑动时触发返回回调 (类似iOS侧滑返回)。
 */
#ifndef UI_GESTURE_H
#define UI_GESTURE_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在指定屏幕上添加侧滑返回手势
 *
 * 在屏幕左侧40px宽的区域创建不可见的触摸检测器,
 * 检测从左到右的滑动手势并调用回调函数。
 *
 * @param screen  目标屏幕对象
 * @param back_cb 返回回调函数
 */
void ui_edge_swipe_add(lv_obj_t *screen, void (*back_cb)(void));

/**
 * @brief 清理所有侧滑手势上下文
 */
void ui_edge_swipe_remove_all(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_GESTURE_H */
