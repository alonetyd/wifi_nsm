/**
 * @file ui_bluetooth.h
 * @brief 蓝牙页面模块
 */
#ifndef UI_BLUETOOTH_H
#define UI_BLUETOOTH_H

#include "lvgl.h"
#include "bsp_bt.h"
#include "bsp_wireless_mgr.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建蓝牙页面
 * @return 蓝牙页面对象
 */
lv_obj_t *ui_bluetooth_create_page(void);

/**
 * @brief 获取蓝牙页面
 */
lv_obj_t *ui_bluetooth_get_screen(void);

/**
 * @brief 刷新蓝牙页面状态
 */
void refresh_bt_page(void);

/**
 * @brief 销毁页面 (主题切换时使用)
 */
void ui_bluetooth_invalidate(void);

/**
 * @brief 显示"已配对设备"操作弹窗 (连接 / 取消配对)
 *
 * 从"已配对设备"列表中点击某个设备时调用.
 *
 * @param dev 指向已配对设备缓存中的元素
 */
void ui_bluetooth_show_paired_action_dialog(bt_paired_dev_t *dev);

/**
 * @brief 无线管理器事件处理 (蓝牙协议栈启停结果, LVGL 线程调用)
 *
 * 由 ui_wireless_bridge 在 LVGL 线程中转发, 更新页面开关与状态.
 *
 * @param evt 事件类型 (BT_STARTED / BT_START_FAIL / BT_STOPPED)
 * @param err 失败原因 (成功时为 ESP_OK)
 */
void ui_bluetooth_on_stack_event(wless_event_t evt, esp_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* UI_BLUETOOTH_H */
