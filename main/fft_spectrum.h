/**
 * @file fft_spectrum.h
 * @brief 音频频谱可视化 (FFT)
 *
 * 由播放任务喂入 stereo PCM, 由 LVGL 定时器计算频谱并取条形高度.
 * 线程安全: feed 与 compute 使用双缓冲, 允许轻微的撕裂 (纯视觉).
 */
#ifndef FFT_SPECTRUM_H
#define FFT_SPECTRUM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FFT_BAR_MAX  24   /* 频谱条形数 */

/**
 * @brief 初始化频谱模块
 * @param num_bars 条形数 (1-FFT_BAR_MAX)
 */
void fft_spectrum_init(int num_bars);

/**
 * @brief 喂入 stereo PCM (播放任务线程调用, 可每次 ~4096 帧)
 * @param stereo 交错 stereo int16
 * @param frames 帧数
 */
void fft_spectrum_feed(const int16_t *stereo, int frames);

/**
 * @brief 计算频谱 (LVGL 定时器调用, 建议 30-60ms 一次)
 */
void fft_spectrum_compute(void);

/**
 * @brief 获取条形高度 (0-255)
 */
uint8_t fft_spectrum_get_bar(int i);

#ifdef __cplusplus
}
#endif

#endif /* FFT_SPECTRUM_H */
