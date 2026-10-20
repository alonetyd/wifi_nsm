/**
 * @file ui_wifi.h
 * @brief WiFi页面模块
 */
#ifndef UI_WIFI_H
#define UI_WIFI_H

#include "lvgl.h"
#include "bsp_wireless_mgr.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建WiFi页面
 * @return WiFi页面对象
 */
lv_obj_t *ui_wifi_create_page(void);

/**
 * @brief 获取WiFi页面
 */
lv_obj_t *ui_wifi_get_screen(void);

/**
 * @brief 刷新WiFi页面状态
 */
void refresh_wifi_page(void);

/**
 * @brief WiFi扫描完成回调 (由bsp_wifi在WIFI_EVENT_SCAN_DONE中触发)
 */
void ui_wifi_on_scan_done(void);

/**
 * @brief 无线管理器事件处理 (WiFi协议栈启停结果, LVGL 线程调用)
 *
 * 由 ui_wireless_bridge 在 LVGL 线程中转发, 更新页面开关与状态.
 *
 * @param evt 事件类型 (WIFI_STARTED / WIFI_START_FAIL / WIFI_STOPPED)
 * @param err 失败原因 (成功时为 ESP_OK)
 */
void ui_wifi_on_stack_event(wless_event_t evt, esp_err_t err);

/**
 * @brief 销毁页面 (主题切换时使用)
 */
void ui_wifi_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_WIFI_H */
