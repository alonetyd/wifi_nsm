/*******************************************************************************
 * SD卡文件管理器 UI 模块 - LVGL 9.x
 *
 * 适用于 ESP32-P4 项目，屏幕分辨率 262×928（竖屏）
 * 颜色格式: RGB565
 *
 * 功能:
 *   - 浏览 SD 卡 FAT 文件系统
 *   - 显示当前路径
 *   - 文件/文件夹可滚动列表
 *   - 点击文件夹进入子目录
 *   - 返回上级目录（根目录返回桌面）
 *   - 点击 .wav 音乐文件直接播放
 *   - 显示文件大小
 ******************************************************************************/

#ifndef UI_FILE_MANAGER_H
#define UI_FILE_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "esp_2_41ttf.h"
#include "bsp_board.h"
#include "bsp_sd_card.h"
#include "audio_player.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>

/* 显示文件管理器页面 (创建并加载独立 screen) */
void ui_file_manager_show(void);

/* 刷新当前目录列表 (重新扫描并填充) */
void ui_file_manager_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_FILE_MANAGER_H */
