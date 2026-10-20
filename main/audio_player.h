/*******************************************************************************
 * Audio Player API - 音频播放器接口声明
 *
 * 支持:
 *   - WAV/MP3 播放, BT A2DP + I2S 双输出
 *   - 播放模式: 顺序 / 随机 / 单曲循环 / 列表循环
 *   - ID3 元数据 (标题/歌手), LRC 歌词, 收藏夹, 睡眠定时
 *   - 记忆播放 (索引 + 秒数 + 模式)
 ******************************************************************************/

#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 音频播放状态 */
typedef enum {
    AUDIO_STATE_IDLE = 0,    /* 空闲 */
    AUDIO_STATE_PLAYING,     /* 播放中 */
    AUDIO_STATE_PAUSED,      /* 已暂停 */
    AUDIO_STATE_STOPPED,     /* 已停止 */
    AUDIO_STATE_BUFFERING,   /* 准备中: 已接受播放/切歌命令, 解码器打开中, 音频尚未出声 */
} audio_state_t;

/* 播放模式 */
typedef enum {
    PLAY_MODE_SEQUENCE = 0,  /* 顺序播放, 播完停止 */
    PLAY_MODE_SHUFFLE,       /* 随机播放 */
    PLAY_MODE_REPEAT_ONE,    /* 单曲循环 */
    PLAY_MODE_REPEAT_ALL,    /* 列表循环 */
} play_mode_t;

/* ---- 播放控制 ---- */
void audio_player_play(void);              /* 开始/恢复播放 */
void audio_player_pause(void);             /* 暂停 */
void audio_player_resume(void);            /* 恢复播放 */
void audio_player_stop(void);              /* 停止播放 */
void audio_player_next(void);              /* 下一曲 */
void audio_player_prev(void);              /* 上一曲 */
void audio_player_play_index(int index);   /* 播放指定索引 */

/* ---- 播放模式 ---- */
void audio_player_set_mode(play_mode_t mode);   /* 设置播放模式 (持久化) */
play_mode_t audio_player_get_mode(void);

/* ---- 参数设置 ---- */
void audio_player_set_volume(uint8_t volume);   /* 设置音量 0-100 */

/* ---- 状态查询 ---- */
audio_state_t audio_player_get_state(void);
int audio_player_get_current_index(void);

/* ---- 初始化与扫描 ---- */
esp_err_t audio_player_init(void);                              /* 初始化音频播放器 */
esp_err_t audio_player_scan_music(const char *dir_path);        /* 扫描目录中的音乐文件 (同步, 文件管理器用) */

/**
 * @brief 后台扫描音乐目录 (开机用, 不阻塞调用线程)
 *
 * 固定扫描 MUSIC_DIR_PATH, 完成后经 scan_done 回调通知 (回调运行在
 * 扫描任务上下文)。重复调用在扫描期间返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t audio_player_scan_music_async(const char *dir_path);
bool audio_player_is_scanning(void);                            /* 是否正在扫描 */
void audio_player_set_scan_done_cb(void (*cb)(int count));      /* 扫描完成回调 */
int audio_player_get_music_count(void);                         /* 获取音乐文件数量 */
const char* audio_player_get_music_name(int index);             /* 获取指定索引的文件名 */

/**
 * @brief 填充播放列表 (便捷接口, 给UI调用)
 * @param out_names  输出字符串数组 (调用方分配, 每项至少 MAX_MUSIC_NAME_LEN 字节)
 * @param max        数组最大容量
 * @return 实际填充数量
 */
int audio_player_fill_names(char (*out_names)[128], int max);

/* ---- ID3 元数据 ---- */
const char *audio_player_get_title(int index);   /* 标题 (无标签则用文件名) */
const char *audio_player_get_artist(int index);  /* 歌手 (可能为空) */

/* ---- LRC 歌词 ---- */
int  audio_player_load_lyrics(int index);             /* 加载歌词, 返回行数 */
int  audio_player_get_lyric_count(void);
int  audio_player_get_lyric_line(uint32_t ms);        /* 当前应高亮的行索引 */
const char *audio_player_get_lyric_text(int line);    /* 指定行文本 */

/* ---- 收藏夹 ---- */
void audio_player_favorite_init(void);        /* 从SD卡加载收藏 */
bool audio_player_is_favorite(int index);
void audio_player_toggle_favorite(int index); /* 切换收藏并保存 */

/* ---- 睡眠定时 ---- */
void audio_player_set_sleep(uint32_t minutes);   /* 0 = 取消 */
uint32_t audio_player_get_sleep_remaining(void); /* 剩余秒数, 0 = 未设置 */

/* ---- 进度回调 ---- */
void audio_player_set_progress_cb(void (*cb)(int index, uint32_t cur,
                                             uint32_t total, const char *title));
/* 曲目开始回调 (UI 用于更新高亮/封面/歌词) */
void audio_player_set_track_cb(void (*cb)(int index));
/* 播放状态变化回调 (BUFFERING/PLAYING/PAUSED/STOPPED/IDLE, 播放任务线程上下文) */
void audio_player_set_state_cb(void (*cb)(audio_state_t state));

/* ---- 记忆播放 ---- */
void audio_player_save_progress(void);           /* 手动保存当前播放进度 */
bool audio_player_load_progress(int *index, uint32_t *seconds); /* 加载上次播放进度 */
void audio_player_seek(uint32_t seconds);        /* 跳转到指定位置(秒) */

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PLAYER_H */
