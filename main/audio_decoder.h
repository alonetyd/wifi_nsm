#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* 音频格式类型 */
typedef enum {
    AUDIO_FMT_UNKNOWN = 0,
    AUDIO_FMT_WAV,
    AUDIO_FMT_MP3,
} audio_fmt_t;

/* 解码器状态 */
typedef struct {
    audio_fmt_t format;         /* 音频格式 */
    uint32_t sample_rate;       /* 采样率 */
    uint16_t channels;          /* 声道数 */
    uint16_t bits_per_sample;   /* 位深 */
    uint32_t total_samples;     /* 总采样数(估算) */
    uint32_t total_seconds;     /* 总时长(秒) */
    uint32_t current_seconds;   /* 当前播放位置(秒) */
} audio_info_t;

/* 打开音频文件并自动识别格式 */
bool audio_decoder_open(const char *filepath, audio_info_t *info);

/* 读取解码后的PCM数据, 返回读取的帧数 (1 帧 = channels 个 16bit 采样 = channels*2 字节)
 * 如果文件是单声道, 内部已自动复制为立体声输出 (L=R).
 * 调用方需要保证 pcm_buf 足够大, 至少容纳 max_frames * channels_out * 2 字节,
 * 其中 channels_out >= 解码器声道数 (本接口确保输出始终为双声道立体声). */
int audio_decoder_read(int16_t *pcm_buf, int max_frames);

/* 跳转到指定位置(秒) */
bool audio_decoder_seek(uint32_t seconds);

/* 关闭解码器 */
void audio_decoder_close(void);

/* 获取当前播放信息 */
bool audio_decoder_get_info(audio_info_t *info);

/* 根据文件扩展名判断格式 */
audio_fmt_t audio_decoder_detect_format(const char *filename);

#endif
