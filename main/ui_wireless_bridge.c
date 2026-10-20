/**
 * @file ui_wireless_bridge.c
 * @brief 无线管理器事件 -> LVGL 线程桥接实现
 *
 * 线程模型:
 *   worker 线程 (wless_mgr) --mgr_event_cb--> 打包消息 + lv_async_call
 *   LVGL 线程 --async_dispatch--> ui_bluetooth/ui_wifi 事件处理函数
 *
 * 与 ui_bluetooth.c 中 dispatch_bt_event 使用相同的 trampoline 模式。
 */

#include "ui_wireless_bridge.h"
#include "ui_lvgl_async.h"
#include "bsp_wireless_mgr.h"
#include "ui_bluetooth.h"
#include "ui_wifi.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdbool.h>

static const char *TAG = "wless_ui_bridge";

/* worker 线程 -> LVGL 线程 的事件消息 */
typedef struct {
    wless_event_t evt;
    esp_err_t     err;
} wless_ui_msg_t;

/* 静态环形消息池: 固定 8 条, 避免每次事件都做小块 heap 分配.
 * 生产者 = wless worker 线程 (单线程), 消费者 = LVGL 线程 (单线程),
 * 槽位标志为单字节读写, 跨线程安全. 池满时丢弃事件 (栈启停事件极低频). */
#define WLESS_MSG_POOL_LEN 8

static wless_ui_msg_t s_msg_pool[WLESS_MSG_POOL_LEN];
static volatile bool s_msg_used[WLESS_MSG_POOL_LEN];
static int s_msg_next = 0;

static wless_ui_msg_t *msg_pool_alloc(void)
{
    for (int i = 0; i < WLESS_MSG_POOL_LEN; i++) {
        int idx = (s_msg_next + i) % WLESS_MSG_POOL_LEN;
        if (!s_msg_used[idx]) {
            s_msg_used[idx] = true;
            s_msg_next = (idx + 1) % WLESS_MSG_POOL_LEN;
            return &s_msg_pool[idx];
        }
    }
    return NULL;
}

static void msg_pool_release(wless_ui_msg_t *msg)
{
    if (msg && msg >= s_msg_pool && msg < s_msg_pool + WLESS_MSG_POOL_LEN) {
        s_msg_used[msg - s_msg_pool] = false;
    }
}

static bool s_initialized = false;

/* 以下运行在 LVGL 线程 */
static void async_dispatch(void *p)
{
    wless_ui_msg_t *msg = (wless_ui_msg_t *)p;
    if (!msg) return;

    switch (msg->evt) {
    case WLESS_EVT_BT_STARTED:
    case WLESS_EVT_BT_START_FAIL:
    case WLESS_EVT_BT_STOPPED:
        ui_bluetooth_on_stack_event(msg->evt, msg->err);
        break;

    case WLESS_EVT_WIFI_STARTED:
    case WLESS_EVT_WIFI_START_FAIL:
    case WLESS_EVT_WIFI_STOPPED:
        ui_wifi_on_stack_event(msg->evt, msg->err);
        break;

    case WLESS_EVT_LINK_UP:
        ESP_LOGI(TAG, "从机链路已建立 (SDIO)");
        break;
    case WLESS_EVT_LINK_FAIL:
        ESP_LOGW(TAG, "从机链路建立失败");
        break;
    default:
        break;
    }

    msg_pool_release(msg);
}

/* 运行在 worker 线程上下文: 只打包 + 异步投递, 不碰 LVGL */
static void mgr_event_trampoline(wless_event_t evt, esp_err_t err)
{
    wless_ui_msg_t *msg = msg_pool_alloc();
    if (!msg) {
        ESP_LOGE(TAG, "事件消息池已满 (evt=%d)", (int)evt);
        return;
    }
    msg->evt = evt;
    msg->err = err;
    ui_lvgl_async_call(async_dispatch, msg);
}

void ui_wireless_bridge_init(void)
{
    if (s_initialized) return;
    bsp_wireless_set_event_cb(mgr_event_trampoline);
    s_initialized = true;
    ESP_LOGI(TAG, "无线管理器事件桥接已初始化");
}
