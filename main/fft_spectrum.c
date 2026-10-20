/**
 * @file fft_spectrum.c
 * @brief 音频频谱可视化实现
 *
 * - 256 点 Radix-2 FFT (浮点, P4 有 FPU)
 * - 对数频率映射到 24 条
 * - 攻击快 / 衰减慢的峰值平滑 (视觉美观)
 * - 双缓冲避免 feed/compute 线程竞争
 */

#include "fft_spectrum.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "bsp_memory.h"

#define FFT_SIZE   256
#define CAP_SIZE   1024          /* 单声道采样捕获环大小 */
#define FEED_DECIM  6            /* 降采样: 44.1k/6 ≈ 7.35kHz 分析带宽 */

static int    s_num_bars = 24;
static int16_t *s_cap = NULL;        /* 双缓冲 (PSRAM): [0] 正在写 / [1] 上一块 */
static volatile int  s_writing = 0;  /* 当前正在被 feed 写入的缓冲 */
static int    s_cap_pos = 0;         /* 当前写入位置 */
static uint8_t s_bars[FFT_BAR_MAX];

/* 浮点 FFT 工作缓冲 (PSRAM) */
static float  *s_real = NULL;
static float  *s_imag = NULL;
static float  *s_win = NULL;         /* Hann 窗 (计算时生成) */
static uint8_t s_bar_cur[FFT_BAR_MAX];
static uint8_t s_bar_peak[FFT_BAR_MAX];

void fft_spectrum_init(int num_bars)
{
    if (num_bars < 1) num_bars = 1;
    if (num_bars > FFT_BAR_MAX) num_bars = FFT_BAR_MAX;
    s_num_bars = num_bars;
    s_cap_pos = 0;
    s_writing = 0;

    /* 大缓冲分配到 PSRAM, 避免挤占内部 DRAM (统一走 bsp_memory) */
    if (s_cap == NULL) {
        s_cap = (int16_t *)bsp_memory_malloc(2 * CAP_SIZE * sizeof(int16_t));
    }
    if (s_real == NULL) {
        s_real = (float *)bsp_memory_malloc(FFT_SIZE * sizeof(float));
    }
    if (s_imag == NULL) {
        s_imag = (float *)bsp_memory_malloc(FFT_SIZE * sizeof(float));
    }
    if (s_win == NULL) {
        s_win = (float *)bsp_memory_malloc(FFT_SIZE * sizeof(float));
    }

    if (s_cap)  memset(s_cap, 0, 2 * CAP_SIZE * sizeof(int16_t));
    if (s_real) memset(s_real, 0, FFT_SIZE * sizeof(float));
    if (s_imag) memset(s_imag, 0, FFT_SIZE * sizeof(float));
    memset(s_bars, 0, sizeof(s_bars));
    memset(s_bar_cur, 0, sizeof(s_bar_cur));
    memset(s_bar_peak, 0, sizeof(s_bar_peak));

    /* Hann 窗预计算 */
    if (s_win) {
        for (int i = 0; i < FFT_SIZE; i++) {
            s_win[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1)));
        }
    }
}

/* 播放任务线程调用 */
void fft_spectrum_feed(const int16_t *stereo, int frames)
{
    if (!stereo || frames <= 0 || s_cap == NULL) return;

    int buf = s_writing;
    int16_t *cap = s_cap + buf * CAP_SIZE;   /* 双缓冲: [0] 正在写 / [1] 上一块 */

    for (int i = 0; i < frames && s_cap_pos < CAP_SIZE; i++) {
        if ((i % FEED_DECIM) == 0) {
            /* 混单声道 */
            int32_t l = stereo[i * 2];
            int32_t r = stereo[i * 2 + 1];
            cap[s_cap_pos++] = (int16_t)((l + r) >> 1);
        }
    }

    if (s_cap_pos >= CAP_SIZE) {
        /* 换缓冲: 通知 compute 读另一块 */
        s_cap_pos = 0;
        s_writing = (s_writing == 0) ? 1 : 0;
        memset(s_cap + s_writing * CAP_SIZE, 0, CAP_SIZE * sizeof(int16_t));
    }
}

/* 就地 Radix-2 迭代 FFT (实部/虚部分离, 位反转后蝶形) */
static void fft_radix2(float *re, float *im, int n)
{
    /* 位反转 */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len;
        float wlen_r = cosf(ang);
        float wlen_i = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float wr = 1.0f, wi = 0.0f;
            int half = len >> 1;
            for (int j = 0; j < half; j++) {
                int a = i + j;
                int b = i + j + half;
                float ur = re[b], ui = im[b];
                float xr = ur * wr - ui * wi;
                float xi = ur * wi + ui * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
                float nwr = wr * wlen_r - wi * wlen_i;
                wi = wr * wlen_i + wi * wlen_r;
                wr = nwr;
            }
        }
    }
}

/* 幂律缩放: 幅度 -> 0-255 (对数值) */
static uint8_t amp_to_bar(float amp)
{
    if (amp <= 0.0f) return 0;
    /* 归一化 + 对数压缩, 视觉友好 */
    float db = 20.0f * log10f(amp + 1e-6f);
    /* FFT_SIZE 满幅正弦峰值约 FFT_SIZE/2 = 128, 对应 42dB */
    float v = (db + 10.0f) / 42.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (uint8_t)(v * 255.0f);
}

/* LVGL 定时器调用 */
void fft_spectrum_compute(void)
{
    if (s_cap == NULL || s_real == NULL || s_imag == NULL || s_win == NULL) return;

    /* 取上一次完整写入的缓冲 (另一块) */
    int read_buf = (s_writing == 0) ? 1 : 0;
    const int16_t *cap = s_cap + read_buf * CAP_SIZE;

    /* 取最后 FFT_SIZE 个采样, 加窗 */
    for (int i = 0; i < FFT_SIZE; i++) {
        int idx = CAP_SIZE - FFT_SIZE + i;
        s_real[i] = (float)cap[idx] * s_win[i];
        s_imag[i] = 0.0f;
    }

    fft_radix2(s_real, s_imag, FFT_SIZE);

    /* 对数频率映射: bin 1..FFT_SIZE/2 */
    const int half = FFT_SIZE / 2;
    for (int b = 0; b < s_num_bars; b++) {
        float f0 = (float)b / s_num_bars;
        float f1 = (float)(b + 1) / s_num_bars;
        /* 对数: bin = half * (k^2), k in [0.01,1] */
        float k0 = 0.01f + 0.99f * f0 * f0;
        float k1 = 0.01f + 0.99f * f1 * f1;
        int i0 = (int)(k0 * (half - 1));
        int i1 = (int)(k1 * (half - 1));
        if (i1 >= half) i1 = half - 1;
        if (i1 < i0) i1 = i0;

        float peak = 0.0f;
        for (int i = i0; i <= i1; i++) {
            float mag = sqrtf(s_real[i] * s_real[i] + s_imag[i] * s_imag[i]);
            if (mag > peak) peak = mag;
        }
        uint8_t v = amp_to_bar(peak);

        /* 平滑: 上升快, 下降慢 */
        if (v > s_bar_cur[b]) {
            s_bar_cur[b] = (uint8_t)((s_bar_cur[b] + v * 2) / 3);
        } else {
            s_bar_cur[b] = (s_bar_cur[b] > 1) ? (s_bar_cur[b] - 1) : 0;
        }
        /* 峰值条 */
        if (v >= s_bar_peak[b]) {
            s_bar_peak[b] = v;
        } else {
            s_bar_peak[b] = (s_bar_peak[b] > 2) ? (s_bar_peak[b] - 2) : 0;
        }
        s_bars[b] = s_bar_cur[b];
    }
}

uint8_t fft_spectrum_get_bar(int i)
{
    if (i < 0 || i >= s_num_bars) return 0;
    return s_bars[i];
}
