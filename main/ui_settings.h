/*******************************************************************************
 * 设置页面 UI 模块 - LVGL 9.x
 *
 * 适用于 ESP32-P4 项目，屏幕分辨率 262×928（竖屏）
 * 包含主设置列表页和5个二级子页面: 蓝牙/WiFi/亮度/音量/关于
 ******************************************************************************/

#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "esp_2_41ttf.h"
#include "bsp_board.h"
#include "bsp_brightness.h"
#include "bsp_audio.h"
#include "bsp_wifi.h"
#include "bsp_bt.h"
#include "bsp_sd_card.h"
#include "audio_player.h"

/* 显示设置主页面 */
void ui_settings_show(void);

/* 获取设置主页面 */
lv_obj_t *ui_settings_get_screen(void);

/* 更新 WiFi 状态 (供 WiFi 事件回调调用) */
void ui_settings_update_wifi(bool connected, const char *ip);

/* 更新蓝牙状态 (供蓝牙事件回调调用) */
void ui_settings_update_bt(bool connected, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* UI_SETTINGS_H */
