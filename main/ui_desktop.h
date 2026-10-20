/*******************************************************************************
 * 小米手环风格桌面 UI 模块 - LVGL 9.x
 *
 * 适用于 ESP32-P4 项目，屏幕分辨率 262×928（竖屏窄长）
 * 包含: 表盘页面 + 下拉控制中心 + 应用导航
 ******************************************************************************/

#ifndef UI_DESKTOP_H
#define UI_DESKTOP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdbool.h>

/**
 * @brief 初始化桌面
 *
 * 创建桌面屏幕、表盘、状态栏、应用栏及下拉控制中心，
 * 并加载桌面屏幕。需在持有 LVGL 锁时调用。
 */
void ui_desktop_init(void);

/**
 * @brief 显示桌面
 *
 * 加载桌面屏幕（用于子页面返回桌面）。
 */
void ui_desktop_show(void);

/**
 * @brief 更新 WiFi 状态
 *
 * 同步状态栏图标颜色和控制中心开关状态。
 * @param connected true 已连接, false 未连接
 */
void ui_desktop_update_wifi(bool connected);

/**
 * @brief 更新蓝牙状态
 *
 * 同步状态栏图标颜色和控制中心开关状态。
 * @param connected true 已连接, false 未连接
 */
void ui_desktop_update_bt(bool connected);

/**
 * @brief 刷新时间显示
 *
 * 更新状态栏时间和表盘大号时间/日期。
 */
void ui_desktop_update_time(void);

/**
 * @brief 显示下拉控制中心
 *
 * 带动画从顶部滑出控制中心面板。
 */
void ui_desktop_show_control_center(void);

/**
 * @brief 隐藏下拉控制中心
 *
 * 带动画将控制中心面板滑回顶部并隐藏。
 */
void ui_desktop_hide_control_center(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_DESKTOP_H */
