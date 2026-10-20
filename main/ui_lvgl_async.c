/**
 * @file ui_lvgl_async.c
 * @brief 线程安全的 lv_async_call 包装实现 (见头文件说明)
 */

#include "ui_lvgl_async.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "ui_lvgl_async";

void ui_lvgl_async_call(lv_async_cb_t cb, void *user_data)
{
    if (cb == NULL) return;

    /* 等待上界 = 端口任务当前这轮 lv_timer_handler (含一次渲染) 的剩余
     * 时间, 通常 <1 帧 (22ms)。递归互斥锁, LVGL 线程内调用亦安全。 */
    if (!lvgl_port_lock(500)) {
        /* 极端拥塞: 放弃本次投递 (调用方均为周期性事件, 下一周期会再来) */
        ESP_LOGW(TAG, "lvgl lock 获取超时, 丢弃一次 async 投递");
        return;
    }
    lv_async_call(cb, user_data);
    lvgl_port_unlock();
}
