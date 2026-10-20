/*******************************************************************************
 * 音乐播放器 UI 模块 - LVGL 9.x
 *
 * 适用于 ESP32-P4 项目，屏幕分辨率 262×928（竖屏）
 * 颜色格式: RGB565
 ******************************************************************************/

#ifndef UI_MUSIC_PLAYER_H
#define UI_MUSIC_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "audio_player.h"
#include "esp_2_41ttf.h"

/* 初始化音乐播放器 UI */
void ui_music_player_init(void);

/* 显示音乐播放器页面(从桌面进入) */
void ui_music_player_show(void);

/* 更新播放状态 */
void ui_music_player_update_state(audio_state_t state);

/* 更新进度 (单位: 秒) */
void ui_music_player_update_progress(uint32_t current_sec, uint32_t total_sec);

/* 更新当前歌曲信息 */
void ui_music_player_update_song(const char *title, const char *artist);

/* 更新播放列表
 * names:        歌曲名数组
 * count:        歌曲数量
 * current_index:当前播放索引 (-1 表示无)
 */
void ui_music_player_update_playlist(const char **names, int count, int current_index);

/* 更新 WiFi 状态 */
void ui_music_player_update_wifi(bool connected, const char *ip);

/* 更新蓝牙状态 */
void ui_music_player_update_bt(bool connected);

/* 设置亮度滑块值 (0-255) */
void ui_music_player_set_brightness_slider(uint8_t value);

/* 设置音量滑块值 (0-100) */
void ui_music_player_set_volume_slider(uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* UI_MUSIC_PLAYER_H */
