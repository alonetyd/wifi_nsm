/**
 * @file ui_wireless_bridge.h
 * @brief 无线管理器事件 -> LVGL 线程桥接
 *
 * bsp_wireless_mgr 的事件回调运行在 worker 线程上下文, 不能直接操作
 * LVGL 对象。本模块注册为唯一的管理器事件回调, 把事件用 malloc 消息 +
 * lv_async_call 中转到 LVGL 线程, 再分发给蓝牙页 / WiFi 页的处理函数
 * (ui_bluetooth_on_stack_event / ui_wifi_on_stack_event)。
 *
 * 在 app_main 中 bsp_wireless_mgr_init() 之后调用一次
 * ui_wireless_bridge_init() 即可。
 */

#ifndef UI_WIRELESS_BRIDGE_H
#define UI_WIRELESS_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化桥接 (注册无线管理器事件回调)
 *
 * 幂等, 重复调用无副作用。
 */
void ui_wireless_bridge_init(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_WIRELESS_BRIDGE_H */
