/**
 * @file ui_lvgl_async.h
 * @brief 线程安全的 lv_async_call 包装
 *
 * esp_lvgl_port 要求: 除 LVGL 端口任务外, 任何任务访问 LVGL API 都必须
 * 持 lvgl_port_lock()。裸调 lv_async_call 会在 LVGL 端口任务遍历/修改
 * 定时器链表时并发插入节点, 损坏链表 (症状: 触摸/界面冻结, 定时器停止)。
 * LVGL 与音频/无线任务同核 (core 1) 后, 同核抢占使该竞态从"偶发"变为
 * "必现", 所有跨线程投递必须经由本包装。
 *
 * 使用方法: 在任意任务线程调用 ui_lvgl_async_call(cb, arg) 代替
 * lv_async_call(cb, arg); 回调本体在 LVGL 线程执行, 无需再加锁。
 * 本包装可在 LVGL 线程内安全调用 (lvgl_port_lock 为递归互斥锁)。
 */

#ifndef UI_LVGL_ASYNC_H
#define UI_LVGL_ASYNC_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 线程安全地把回调排入 LVGL 线程异步执行
 *
 * @param cb        LVGL 线程中执行的回调
 * @param user_data 透传给回调的参数
 */
void ui_lvgl_async_call(lv_async_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* UI_LVGL_ASYNC_H */
