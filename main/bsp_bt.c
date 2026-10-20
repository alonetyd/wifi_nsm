/**
 * @file bsp_bt.c
 * @brief 蓝牙模块 - 通过ESP-Hosted使用经典蓝牙A2DP + BLE 4.2
 *
 * ESP32-P4通过ESP-Hosted SDIO连接ESP32协处理器，支持:
 * - 经典蓝牙 A2DP Source (向蓝牙耳机发送音频)
 * - BLE 4.2 扫描 (发现BLE设备)
 * 不支持 BLE 5.0 (ESP32协处理器硬件限制)。
 */

#include "bsp_bt.h"
#include "bsp_board.h"
#include "bsp_wifi.h"
#include "bsp_audio.h"  /* 用于 bsp_audio_set_volume() 同步音量到 CS43131 */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#if (esp_idf_version >= ESP_IDF_VERSION_VAL(5, 2, 0))
#include "freertos/idf_additions.h"
#endif

static const char *TAG = "bsp_bt";

#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED)

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_gap_ble_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_hosted_hci_bluedroid.h"
#if defined(CONFIG_BT_A2DP_ENABLE)
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#endif
#include "freertos/timers.h"

#define BT_DEVICE_NAME "ESP32-P4-Player"
static size_t s_pcm_ring_fill = 0;
static uint8_t *s_pcm_ring_buf = NULL;
static size_t s_pcm_ring_wr = 0;
static size_t s_pcm_ring_rd = 0;
//static SemaphoreHandle_t s_pcm_ring_mux = NULL;


static bool s_initialized = false;
static StaticSemaphore_t s_init_mux_buf;
static SemaphoreHandle_t s_init_mux = NULL;
static bool s_connected = false;
static bool s_scanning = false;          /* BLE 扫描中 */
static bool s_classic_scanning = false; /* 经典 BT 发现中 */
static bt_event_cb_t s_event_cb = NULL;
static ble_scan_cb_t s_ble_scan_cb = NULL;
static bt_classic_scan_cb_t s_classic_scan_cb = NULL;
static char s_local_addr[18] = {0};
static char s_connected_name[64] = {0};
static char s_connected_addr[18] = {0};    /* 最近成功连接 / 当前正在重连的目标地址 */
static esp_bd_addr_t s_connected_bda = {0}; /* 真实 BDA, 用于发起 A2DP 连接 */
static void bt_work_task_main(void *arg);
/* 用于主动 ACL 断开请求 */
static bool s_user_requested_disconnect = false;

/* 自动重连 */
static bool s_auto_reconnect_enabled = true;
static int  s_reconnect_attempts = 0;
static TimerHandle_t s_reconnect_timer = NULL;
/* 修后: 重连 backoff 起点调到 2s, 给 ESP-Hosted 链路更多恢复时间. */
static const int s_reconnect_backoff_ms[] = { 2000, 4000, 8000, 15000, 30000, 60000 };
static const int s_reconnect_backoff_count = sizeof(s_reconnect_backoff_ms) / sizeof(s_reconnect_backoff_ms[0]);

/* A2DP 连接请求看门狗:
 * esp_a2d_source_connect() 发出后, 若 20s 内既没有 CONNECTED 也没有
 * DISCONNECTED 事件 (远端不响应 / ESP-Hosted HCI 吞事件), 连接流程会
 * 无限期卡住. 看门狗到期后把目标重新排入 backoff 重连流程. */
#define BT_CONNECT_WATCHDOG_MS   20000
static TimerHandle_t s_connect_watchdog = NULL;
static void bt_connect_watchdog_cb(TimerHandle_t tmr);
static void bt_connect_watchdog_arm(void);
static void bt_connect_watchdog_disarm(void);

/* 开启蓝牙后自动回连上次设备 (NVS 持久化) */
static bool s_auto_connect_last = true;

/* 连接失败连击计数: 连续 esp_a2d_source_connect() 同步失败达到阈值,
 * 说明 ESP-Hosted 链路大概率异常, 升级为 SDIO 深度恢复 */
static int s_connect_fail_streak = 0;
#define BT_CONNECT_FAIL_STREAK_MAX   3
/* SDIO 深度恢复循环次数 (防止无限恢复-失败-恢复) */
static int s_recovery_cycles = 0;
#define BT_RECOVERY_CYCLES_MAX       3

/* ================ ESP-Hosted SDIO 卡死检测/恢复 ================
 *
 * 背景: P4 + ESP-Hosted 协处理器共存 WiFi+BT 时, SDIO 链路在流量峰值会
 *       出现 "invalid ret or len_from_slave: 0 0" 锁死. 锁死后所有 HCI
 *       命令 (esp_a2d_media_ctrl / esp_bt_gap_* 等) 会 5-15s 后
 *       "command_timed_out". 此时耳机实际上没真断, 是 ESP-Hosted 没回应.
 *
 * 检测: 记录最近一次 ACL_DISCONN_CMP_EVT (来自远端, rsn 0x13 = 正常)
 *       与最近一次 HCI command_timed_out 之间的时序. 如果 ACL 断之前
 *       5s 内出现过 SDIO 错误 (通过 esp_bt_gap_get_conn_count == 0 但
 *       ACL handle 还存在等状态判断), 标记 s_sdio_recovery_pending=true,
 *       重连 backoff 退避策略改为"等待 30s 再试".
 */
static int64_t  s_last_sdio_err_us      = 0;     /* 上一次 eh_sdio 错误时间 */
static bool     s_sdio_recovery_pending = false; /* true: SDIO 卡死中, 重连需更长间隔 */
#define SDIO_ERR_TIMEOUT_US  (5 * 1000 * 1000LL)  /* ACL 断前 5s 内的 SDIO 错误算关联 */
#define SDIO_RECOVER_COOLDOWN_MS  30000          /* SDIO 卡死后 30s 内不允许重连 */

/* ESP log 钩子: 用来识别 "eh_sdio: invalid ret or len_from_slave: 0 0"
 * 这类 SDIO 链路锁死信号, 一旦出现就标记 s_last_sdio_err_us. 注意这个
 * 钩子在所有线程 (包括 BTC_TASK, SDIO 中断 task) 都会执行, 所以处理必须
 * 极轻 (只更新 int64_t + bool, 不分配内存, 不调 mutex). */
static vprintf_like_t s_orig_log_vprintf = NULL;
static int sdio_log_hook(const char *fmt, va_list args)
{
    /* 用 vprintf 一次"dry-run"打印到 stack buffer, 然后匹配关键字.
     * 这样可以避免硬编码源位置 (ESP-HOSTED 内部 log 路径分散). */
    char buf[160];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n > 0 && n < (int)sizeof(buf)) {
        /* SDIO 链路锁死的特征关键字 (来自 ESP-Hosted 源码) */
        if (strstr(buf, "invalid ret or len_from_slave") != NULL ||
            strstr(buf, "HCI command timeout") != NULL ||
            strstr(buf, "ESP_HOSTED_SDIO_UNRESPONSIVE") != NULL ||
            strstr(buf, "unresponsive") != NULL) {
            s_last_sdio_err_us = esp_timer_get_time();
        }
    }
    if (s_orig_log_vprintf) {
        return s_orig_log_vprintf(fmt, args);
    }
    return vfprintf(stderr, fmt, args);
}

/* ================ BT 延迟工作队列 ================
 *
 * 背景: 某些 Bluedroid API (esp_bt_gap_read_remote_name / esp_a2d_media_ctrl)
 *       通过 btc_transfer_context() 把消息发给 BTC_TASK. 如果在 BTC_TASK
 *       上下文 (例如 A2DP 回调内部) 直接调用, BTC_TASK 会等自己处理, 触发
 *       栈死锁 (在 RISC-V 上表现为 Stack protection fault).
 *
 * 方案: 把这些调用扔到独立的工作线程. 工作线程只在 bsp_bt_init() 中创建,
 *       队列大小 4, 足够吸收 A2DP connect/disconnect 事件.
 */
/* 触发 A2DP CONNECTED 回调的远端名请求:
 * - BT_WORK_RMT_NAME_REQ: 通过 btc_transfer_context 发到 BTC_TASK (异步).
 * - BT_WORK_MEDIA_START:  同上.
 * - BT_WORK_LOOKUP_NAME:  NVS 查已配对名称 (避免在 BTC_TASK 上 2.6KB 栈分配).
 * - BT_WORK_RECOVER_SDIO: SDIO 链路疑似锁死后, 暂停重连 + 复位 BT 控制器.
 */
typedef enum {
    BT_WORK_RMT_NAME_REQ = 0,
    BT_WORK_MEDIA_START,
    BT_WORK_LOOKUP_NAME,
    BT_WORK_RECOVER_SDIO,
    BT_WORK_AUTO_CONNECT,   /* 开启蓝牙后回连上次设备 (NVS) */
    BT_WORK_RECONNECT,      /* 自动重连 timer 到期: 实际连接由工作线程执行 */
} bt_work_type_t;

typedef struct {
    bt_work_type_t type;
    char           addr[18];   /* 远端 BT 地址 (ASCII "xx:xx:...") */
} bt_work_msg_t;

static QueueHandle_t       s_bt_work_queue  = NULL;
static TaskHandle_t        s_bt_work_task   = NULL;
#define BT_WORK_QUEUE_LEN   8
/*
 * BTC_TASK 栈只有约 3.5KB. 在 A2DP CONNECTED 回调里访问 NVS 会触发
 *   bt_paired_dev_t tmp[NVS_KEY_PAIRED_IDX_MAX + 1]; // 32 * 82 = 2624B
 * 再加上自身栈帧 + snprintf, 已经逼近栈底. 这里把工作线程栈放到 PSRAM
 * (ESP-IDF v5.2+ 的 xTaskCreateWithCaps) 并放大到 12KB, 留出充分余量
 * (SDIO 锁死后 BTC 任务回调栈会更深, 必须留足).
 */
/* xTaskCreateWithCaps 的栈深度单位是 StackType_t，不是字节。 */
#define BT_WORK_TASK_STACK_BYTES  (12 * 1024)
#define BT_WORK_TASK_STACK        (BT_WORK_TASK_STACK_BYTES / sizeof(StackType_t))
#define BT_WORK_TASK_PRIO   4
#define BT_WORK_TAG         "bt_work"

/* ================ A2DP Source PCM ring buffer ================
 *
 * 使用无锁 ring buffer 实现，生产者(playback)和消费者(SBC encoder)之间通过
 * portMUX 临界区同步。由于两者都在同一 CPU 核心上运行，临界区非常短暂。
 *
 * 设计原则:
 *   - 使用 portMUX (自旋锁) 替代 semaphore mutex，减少锁开销
 *   - 写操作(playback)和读操作(SBC callback)都进入临界区
 *   - 256KB ring: 约 1.5 秒 @ 44.1k stereo，足以吸收抖动
 *   - 临界区持续时间 < 10us (仅复制几 KB 数据)
 */
#include "freertos/portmacro.h"
#include "esp_timer.h"

#define BT_PCM_RING_SIZE  (512 * 1024)
/* A2DP 连接建立时预填充的静音字节数: 64KB (~360ms @ 44.1k stereo)。
 * 只需盖住 MEDIA_START 到真实音频进入的窗口, 不要填满 ring
 * (旧版填满 256KB 导致真实音频被削顶丢数据 = 播放卡顿的根因之一) */
#define BT_PCM_PREFILL_BYTES  (64 * 1024)
static uint8_t       *s_pcm_ring = NULL;
static size_t        s_pcm_ring_wp = 0;
static size_t        s_pcm_ring_rp = 0;
static portMUX_TYPE  s_pcm_ring_mux = portMUX_INITIALIZER_UNLOCKED;

/* 音量控制: BT 和 I2S 共享同一个音量值 (0-100).
 * BT 路径通过软件增益实现，I2S 路径通过 CS43131 硬件实现.
 * s_volume 在 bsp_audio.c 和 bsp_bt.c 中各有一份副本，
 * 通过 bsp_bt_set_volume() / bsp_bt_get_volume() 保持同步. */
static uint8_t  s_volume = 70;

/* PCM 数据回调的中间缓冲区: Bluedroid SBC encoder 每次拉 512 字节 PCM
 * (SBC 标准帧). 我们用这个中间 buffer 做音量增益，避免每次 push 都要分配.
 * 放在 .bss 段，不占堆.
 *
 * ⚠️ 必须是 512 个元素 (256帧 × 2声道): 旧值 [256] 少了一半, 增益路径
 * 每块写 512 个样本 -> 越界写 512B 破坏相邻 .bss (砸中 s_pcm_ring 指针),
 * 触发 ROM memcpy Load access fault -> panic 重启 (蓝牙播放闪退根因). */
static int16_t s_pcm_gain_buf[512];  /* 256帧 × 2ch = 512样本 = 1024 字节 */

/* 增益路径每块处理的帧数上限: 由 s_pcm_gain_buf 大小推导 (256帧),
 * 保证缓冲与上限永不脱节 (旧版硬编码 256 但缓冲只有 128 帧, 越界写). */
#define BT_GAIN_CHUNK_FRAMES  ((int)(sizeof(s_pcm_gain_buf) / (2 * sizeof(int16_t))))

/* 淡入状态: 在 A2DP 媒体传输启动时做短淡入 (20ms) 消除静音 pop.
 * fade_in_frames: 淡入剩余帧数 (淡入总时长 = FADE_IN_TOTAL_FRAMES / sample_rate)
 * fade_in_factor: 当前淡入增益因子 (0-32767)，每帧递减. */
#define BT_FADE_IN_TOTAL_FRAMES  882  /* ~20ms @ 44.1k */
static int32_t s_fade_in_frames = 0;   /* = 0 表示淡入已完成 */
static int32_t s_fade_in_factor = 32767;  /* 初始目标 = 1.0 */

/* 启动淡入: 在 MEDIA_START 后、音频数据开始推入时调用.
 * 淡入时长 = FADE_IN_TOTAL_FRAMES / sample_rate 秒.
 * s_fade_in_factor 从 1 开始 (非 0)，避免第一块数据完全静音产生 pop 杂音。 */
void bsp_bt_start_fade_in(void)
{
    s_fade_in_frames = BT_FADE_IN_TOTAL_FRAMES;
    s_fade_in_factor = 1;  /* 从 1 开始，第一帧就有微弱声音 */
}

/* 应用音量增益到 PCM 数据 (in-place, stereo interleaved int16).
 * gain: 0-100，对应 PCM 乘以 gain/100.
 * 静音 (gain=0) 或极低音量时直接 memset 0，不做乘法. */
static void apply_volume_gain(int16_t *stereo_pcm, size_t frames, uint8_t gain)
{
    if (gain == 0) {
        memset(stereo_pcm, 0, frames * 4);
        return;
    }
    if (gain >= 100 && s_fade_in_frames == 0) return;  /* 无需增益，原样通过 */

    /* 固定点乘法: volume_factor = gain * 32767 / 100
     * 避免浮点运算，省 CPU. */
    int volume_factor = (int)gain * 32767 / 100;

    size_t count = frames * 2;  /* stereo: L,R,L,R... */
    for (size_t i = 0; i < count; i++) {
        int sample = (int)stereo_pcm[i];
        /* 先应用淡入因子 (如果有) */
        if (s_fade_in_frames > 0) {
            sample = sample * s_fade_in_factor / 32767;
        }
        /* 再应用音量因子 */
        if (volume_factor < 32767) {
            sample = sample * volume_factor / 32767;
        }
        stereo_pcm[i] = (int16_t)sample;
    }

    /* 更新淡入状态 */
    if (s_fade_in_frames > 0) {
        s_fade_in_frames -= (int32_t)frames;
        if (s_fade_in_frames <= 0) {
            s_fade_in_frames = 0;
            s_fade_in_factor = 32767;  /* 淡入完成 */
        } else {
            /* 线性淡入: 每帧增加 32767 / FADE_IN_TOTAL_FRAMES */
            int32_t inc = 32767 / BT_FADE_IN_TOTAL_FRAMES;
            s_fade_in_factor += inc * (int32_t)frames;
            if (s_fade_in_factor > 32767) s_fade_in_factor = 32767;
        }
    }
}

/* 把外部 PCM 推入 ring buffer. 由 audio_player 调用.
 *
 * 设计原则: 非阻塞，立即返回。
 *
 * 哒哒哒杂音的根因分析:
 * - SBC encoder (btc_media_task) 每 ~52ms 调用一次 callback，拉走固定字节
 * - encoder callback 里获取 mutex 等待最多 2ms，超时返回静音
 * - player 线程里 vTaskDelayUntil 暂停期间完全不释放 CPU
 * - 如果 push 也等待 mutex，player 可能和 encoder 同时抢锁
 *   → encoder 等 2ms 超时返回静音 → 丢音频帧 → 哒哒哒
 *
 * 注意: 音量增益/淡入在 push 时处理。如果 volume < 100 或正在淡入，
 * 走逐帧固定点乘法路径 (使用 s_pcm_gain_buf 临时缓冲, 256帧/次)。
 * 如果 gain 为 100 且无淡入，走快速 memcpy 路径。
 *
 * 注意: 现在使用 portMUX 临界区替代 semaphore mutex，减少锁开销。 */
size_t bsp_bt_pcm_ring_push(const uint8_t *data, size_t len)
{
    if (s_pcm_ring == NULL || data == NULL || len == 0) {
        return 0;
    }

    size_t written = 0;

    /* 使用 portMUX 临界区替代 semaphore mutex */
    portENTER_CRITICAL(&s_pcm_ring_mux);

    size_t free_bytes = BT_PCM_RING_SIZE - s_pcm_ring_fill;
    size_t to_write = (len > free_bytes) ? free_bytes : len;

    if (to_write > 0) {
        const uint8_t *src = data;

        /* 检查是否需要音量增益或淡入 */
        bool need_gain = (s_volume < 100) || (s_fade_in_frames > 0);
        if (!need_gain) {
            /* 快速路径: 无需增益，原样 memcpy */
            size_t remaining = to_write;
            while (remaining > 0) {
                size_t chunk = remaining;
                size_t wp_first = BT_PCM_RING_SIZE - s_pcm_ring_wp;
                if (chunk > wp_first) chunk = wp_first;
                memcpy(s_pcm_ring + s_pcm_ring_wp, src, chunk);
                s_pcm_ring_wp = (s_pcm_ring_wp + chunk) % BT_PCM_RING_SIZE;
                src += chunk;
                remaining -= chunk;
            }
        } else {
            /* 增益路径: 逐块做固定点乘法，再写入 ring */
            int gain_factor = (int)s_volume * 32767 / 100;
            size_t remaining = to_write;
            while (remaining > 0) {
                size_t chunk_frames = remaining / 4;
                if (chunk_frames > BT_GAIN_CHUNK_FRAMES) chunk_frames = BT_GAIN_CHUNK_FRAMES;
                size_t chunk_bytes = chunk_frames * 4;

                const int16_t *src16 = (const int16_t *)src;
                size_t count = chunk_frames * 2;
                for (size_t i = 0; i < count; i++) {
                    int sample = (int)src16[i];
                    if (s_fade_in_frames > 0) {
                        sample = (sample * s_fade_in_factor) >> 15;
                    }
                    if (gain_factor < 32767) {
                        sample = (sample * gain_factor) >> 15;
                    }
                    s_pcm_gain_buf[i] = (int16_t)sample;
                }

                /* 更新淡入状态 */
                if (s_fade_in_frames > 0) {
                    s_fade_in_frames -= (int32_t)chunk_frames;
                    if (s_fade_in_frames <= 0) {
                        s_fade_in_frames = 0;
                        s_fade_in_factor = 32767;
                    } else {
                        int32_t inc = 32767 / BT_FADE_IN_TOTAL_FRAMES;
                        s_fade_in_factor += inc * (int32_t)chunk_frames;
                        if (s_fade_in_factor > 32767) s_fade_in_factor = 32767;
                    }
                }

                /* 写入 ring (分两段处理环绕) */
                size_t wp_first = BT_PCM_RING_SIZE - s_pcm_ring_wp;
                size_t write_bytes = chunk_bytes;
                if (write_bytes > wp_first) write_bytes = wp_first;
                memcpy(s_pcm_ring + s_pcm_ring_wp, (const uint8_t *)s_pcm_gain_buf, write_bytes);
                if (write_bytes < chunk_bytes) {
                    memcpy(s_pcm_ring, (const uint8_t *)s_pcm_gain_buf + write_bytes,
                           chunk_bytes - write_bytes);
                }
                s_pcm_ring_wp = (s_pcm_ring_wp + chunk_bytes) % BT_PCM_RING_SIZE;
                src += chunk_bytes;
                remaining -= chunk_bytes;
            }
        }

        s_pcm_ring_fill += to_write;
        written = to_write;
    }

    portEXIT_CRITICAL(&s_pcm_ring_mux);
    return written;
}

/* 查询 ring 当前已填充字节数 (使用 portMUX). */
size_t bsp_bt_pcm_ring_fill(void)
{
    size_t fill;
    portENTER_CRITICAL(&s_pcm_ring_mux);
    fill = s_pcm_ring_fill;
    portEXIT_CRITICAL(&s_pcm_ring_mux);
    return fill;
}

/* 查询 ring 容量. */
size_t bsp_bt_pcm_ring_capacity(void)
{
    return BT_PCM_RING_SIZE;
}

/* 预填充 ring buffer: 用静音 PCM (0) 填 ring.
 * 在 BT 连接后、A2DP 媒体传输启动前调用.
 * 给 SBC encoder 足够的初始缓冲, 避免播放开始后立刻 underflow.
 *
 * @param bytes 要填入的静音字节数 (建议 32KB = 32000+).
 * @return 实际填入的字节数.
 */
size_t bsp_bt_pcm_ring_prefill(size_t bytes)
{
    if (s_pcm_ring == NULL) {
        return 0;
    }

    size_t filled = 0;
    portENTER_CRITICAL(&s_pcm_ring_mux);
    while (filled < bytes) {
        size_t free_bytes = BT_PCM_RING_SIZE - s_pcm_ring_fill;
        if (free_bytes == 0) break;

        size_t to_write = free_bytes < (bytes - filled) ? free_bytes : (bytes - filled);

        /* 静音 PCM 全是 0, 直接写 ring. 分两段处理 (环绕). */
        size_t first = BT_PCM_RING_SIZE - s_pcm_ring_wp;
        if (first > to_write) first = to_write;
        memset(s_pcm_ring + s_pcm_ring_wp, 0, first);
        size_t second = to_write - first;
        if (second > 0) {
            memset(s_pcm_ring, 0, second);
        }

        s_pcm_ring_wp = (s_pcm_ring_wp + to_write) % BT_PCM_RING_SIZE;
        s_pcm_ring_fill += to_write;
        filled += to_write;
    }
    portEXIT_CRITICAL(&s_pcm_ring_mux);
    return filled;
}

/* 阻塞版 push: 在 max_wait_ms 时间内尽量把数据塞进 ring, 不会丢字节 (除非超时).
 * 用于 audio_player 这种实时生产者; 内部循环: 进入临界区 -> 若 free >= 还需 push
 * 的部分则一次性 memcpy; 否则放临界区 sleep 5ms 再重试.
 *
 * 注意: 同时处理音量增益和淡入，当 volume < 100 或正在淡入时，
 * 走逐帧固定点乘法路径 (使用 s_pcm_gain_buf 作为临时缓冲)。 */
size_t bsp_bt_pcm_ring_push_blocking(const uint8_t *data, size_t len, uint32_t max_wait_ms)
{
    if (s_pcm_ring == NULL || data == NULL || len == 0) {
        return 0;
    }

    /* 检查是否需要音量增益或淡入 */
    bool need_gain = (s_volume < 100) || (s_fade_in_frames > 0);
    int gain_factor = need_gain ? ((int)s_volume * 32767 / 100) : 32767;

    size_t total_pushed = 0;
    int64_t deadline_us = esp_timer_get_time() + (int64_t)max_wait_ms * 1000;

    while (total_pushed < len) {
        int64_t now_us = esp_timer_get_time();
        if (now_us >= deadline_us) break;

        /* 进入临界区 */
        portENTER_CRITICAL(&s_pcm_ring_mux);

        size_t free_bytes = BT_PCM_RING_SIZE - s_pcm_ring_fill;
        size_t want = len - total_pushed;
        size_t to_write = (want > free_bytes) ? free_bytes : want;

        if (to_write > 0) {
            const uint8_t *src = data + total_pushed;

            if (!need_gain) {
                /* 快速路径: 无需增益，原样 memcpy */
                size_t first = BT_PCM_RING_SIZE - s_pcm_ring_wp;
                if (first > to_write) first = to_write;
                memcpy(s_pcm_ring + s_pcm_ring_wp, src, first);
                size_t second = to_write - first;
                if (second > 0) memcpy(s_pcm_ring, src + first, second);
            } else {
                /* 增益路径: 逐块做固定点乘法，再写入 ring */
                size_t remaining = to_write;
                while (remaining > 0) {
                    size_t chunk_frames = remaining / 4;
                    if (chunk_frames > BT_GAIN_CHUNK_FRAMES) chunk_frames = BT_GAIN_CHUNK_FRAMES;
                    size_t chunk_bytes = chunk_frames * 4;

                    const int16_t *src16 = (const int16_t *)src;
                    size_t count = chunk_frames * 2;
                    for (size_t i = 0; i < count; i++) {
                        int sample = (int)src16[i];
                        if (s_fade_in_frames > 0) {
                            sample = (sample * s_fade_in_factor) >> 15;
                        }
                        if (gain_factor < 32767) {
                            sample = (sample * gain_factor) >> 15;
                        }
                        s_pcm_gain_buf[i] = (int16_t)sample;
                    }

                    /* 更新淡入状态 */
                    if (s_fade_in_frames > 0) {
                        s_fade_in_frames -= (int32_t)chunk_frames;
                        if (s_fade_in_frames <= 0) {
                            s_fade_in_frames = 0;
                            s_fade_in_factor = 32767;
                            gain_factor = 32767;
                            need_gain = (s_volume < 100) || (s_fade_in_frames > 0);
                        } else {
                            int32_t inc = 32767 / BT_FADE_IN_TOTAL_FRAMES;
                            s_fade_in_factor += inc * (int32_t)chunk_frames;
                            if (s_fade_in_factor > 32767) s_fade_in_factor = 32767;
                        }
                    }

                    /* 写入 ring (分两段处理环绕) */
                    size_t wp_first = BT_PCM_RING_SIZE - s_pcm_ring_wp;
                    size_t write_bytes = chunk_bytes;
                    if (write_bytes > wp_first) write_bytes = wp_first;
                    memcpy(s_pcm_ring + s_pcm_ring_wp, (const uint8_t *)s_pcm_gain_buf, write_bytes);
                    if (write_bytes < chunk_bytes) {
                        memcpy(s_pcm_ring, (const uint8_t *)s_pcm_gain_buf + write_bytes,
                               chunk_bytes - write_bytes);
                    }
                    s_pcm_ring_wp = (s_pcm_ring_wp + chunk_bytes) % BT_PCM_RING_SIZE;
                    src += chunk_bytes;
                    remaining -= chunk_bytes;
                }
            }

            s_pcm_ring_fill += to_write;
            total_pushed += to_write;
        }

        portEXIT_CRITICAL(&s_pcm_ring_mux);

        if (to_write == 0) {
            /* ring 仍满, 短暂 sleep 后重试. */
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return total_pushed;
}

/* 音量 getter/setter: BT 路径使用软件增益，I2S 路径使用 CS43131 硬件.
 * 两侧共享同一个音量值，调用任一 API 都会同步两侧. */
uint8_t bsp_bt_get_volume(void)
{
    return s_volume;
}

void bsp_bt_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    s_volume = volume;
    /* BT 音量使用纯软件衰减 (apply_volume_gain 在 push 时处理)，
     * 不依赖 CS43131 I2C 写入，避免 I2C 总线争用失败。
     * s_volume 直接控制 PCM 数据的增益倍数（0-100 → 0x00-0x78 映射）。 */
}

/* 复位 ring buffer (flush). 由 audio_player 在重新打开文件/暂停时调用. */
void bsp_bt_pcm_ring_reset(void)
{
    portENTER_CRITICAL(&s_pcm_ring_mux);
    s_pcm_ring_wp = 0;
    s_pcm_ring_rp = 0;
    s_pcm_ring_fill = 0;
    portEXIT_CRITICAL(&s_pcm_ring_mux);
}

/* 内部: Bluedroid callback 调用, 从 ring buffer 拉 PCM.
 * 注意: 这个函数在 btc_media_task 上下文执行, 不能阻塞太久.
 *
 * 使用 portMUX 临界区替代 semaphore mutex，因为临界区非常短暂 (<10us)。 */
static int32_t bt_a2dp_src_pcm_cb(uint8_t *buf, int32_t len)
{
    if (buf == NULL || len <= 0) {
        /* len == -1: flush signal */
        bsp_bt_pcm_ring_reset();
        return 0;
    }

    portENTER_CRITICAL(&s_pcm_ring_mux);

    size_t to_read = (size_t)len;
    if (to_read > s_pcm_ring_fill) to_read = s_pcm_ring_fill;

    if (to_read == 0) {
        /* ring 空: 填静音数据，不返回 0 字节。
         * SBC encoder 收到全 0 会发静音帧给耳机，不会触发断连。 */
        memset(buf, 0, (size_t)len);
        portEXIT_CRITICAL(&s_pcm_ring_mux);
        return len;
    }

    /* 分两段读取 (环绕) */
    size_t first = BT_PCM_RING_SIZE - s_pcm_ring_rp;
    if (first > to_read) first = to_read;
    memcpy(buf, s_pcm_ring + s_pcm_ring_rp, first);
    size_t second = to_read - first;
    if (second > 0) memcpy(buf + first, s_pcm_ring, second);

    s_pcm_ring_rp = (s_pcm_ring_rp + first + second) % BT_PCM_RING_SIZE;
    s_pcm_ring_fill -= (first + second);

    /* 如果 ring 不足以填满 len，补静音 */
    if (to_read < (size_t)len) {
        memset(buf + to_read, 0, (size_t)len - to_read);
    }

    portEXIT_CRITICAL(&s_pcm_ring_mux);
    return len;
}

/* 内部 helpers (定义在文件靠后位置, 这里前向声明) */
static esp_err_t parse_addr_string(const char *addr, esp_bd_addr_t out_bda);
static void bt_reconnect_timer_cb(TimerHandle_t tmr);
static void load_paired_from_nvs(void);

/* ================ 已配对设备持久化 (NVS) ================ */

#define NVS_BT_PAIRED_NAMESPACE  "bt_paired"
#define NVS_KEY_PAIRED_COUNT     "count"
#define NVS_KEY_PAIRED_PREFIX    "dev_"   /* dev_0_addr, dev_0_name, dev_1_addr, ... */
#define NVS_KEY_PAIRED_IDX_MAX   31       /* 最多支持 32 个已配对设备 */

static int s_paired_count = 0;          /* 已配对设备数量 (启动时从 NVS 加载) */
static SemaphoreHandle_t s_paired_mutex = NULL;

/* 读一个键到字符串缓冲区 (str 类型 NVS) */
static esp_err_t nvs_get_str_safe(nvs_handle_t h, const char *key,
                                  char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';
    size_t len = buf_len;
    esp_err_t ret = nvs_get_str(h, key, buf, &len);
    if (ret == ESP_OK) {
        if (len > 0 && len <= buf_len) buf[len - 1] = '\0';
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        ret = ESP_OK;
    }
    return ret;
}

/* 从 NVS 加载所有已配对设备到内存缓存 */
static void load_paired_from_nvs(void)
{
    if (!s_paired_mutex) {
        s_paired_mutex = xSemaphoreCreateMutex();
    }
    if (!s_paired_mutex) return;

    xSemaphoreTake(s_paired_mutex, portMAX_DELAY);

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_BT_PAIRED_NAMESPACE, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        /* 命名空间不存在 (尚未保存过任何设备), 这是正常情况 */
        s_paired_count = 0;
        xSemaphoreGive(s_paired_mutex);
        return;
    }

    uint8_t cnt_u8 = 0;
    if (nvs_get_u8(h, NVS_KEY_PAIRED_COUNT, &cnt_u8) == ESP_OK) {
        s_paired_count = cnt_u8;
    } else {
        s_paired_count = 0;
    }
    ESP_LOGI(TAG, "从NVS加载已配对设备数量: %d", s_paired_count);

    xSemaphoreGive(s_paired_mutex);
    nvs_close(h);
}

/* ================ 上次连接设备持久化 (自动回连用) ================ */

#define NVS_KEY_LAST_ADDR   "last_addr"
#define NVS_KEY_LAST_NAME   "last_name"

/* 保存上次成功连接的设备 (在 BT 工作线程调用, 避免占用 BTC_TASK) */
static void save_last_device_to_nvs(const char *addr, const char *name)
{
    if (!addr || !addr[0]) return;

    nvs_handle_t h;
    if (nvs_open(NVS_BT_PAIRED_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, NVS_KEY_LAST_ADDR, addr);
    if (name && name[0]) {
        nvs_set_str(h, NVS_KEY_LAST_NAME, name);
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "已记录上次连接设备: %s (%s)", addr, name ? name : "-");
}

/* 读取上次连接设备到 s_connected_addr (在 bsp_bt_init 中调用) */
static void load_last_device_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_BT_PAIRED_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char addr[18] = {0};
    size_t len = sizeof(addr);
    if (nvs_get_str(h, NVS_KEY_LAST_ADDR, addr, &len) == ESP_OK && addr[0]) {
        strncpy(s_connected_addr, addr, sizeof(s_connected_addr) - 1);
        s_connected_addr[sizeof(s_connected_addr) - 1] = '\0';
        parse_addr_string(addr, s_connected_bda);
        ESP_LOGI(TAG, "上次连接设备: %s (开启后可自动回连)", addr);
    }
    nvs_close(h);
}

/* ================ CoD 音频设备判断 ================ */

/* Class of Device 字段:
 *   bits  23..13  Service Class (Audio bit = 1<<19 = 0x080000)
 *   bits  12..8   Major Device Class (Audio/Video = 0x0400)
 *   bits  7..2    Minor Device Class
 */
#define COD_MAJOR_AUDIO_VIDEO   0x0400
#define COD_MINOR_AUDIO_MASK    0xFC    /* bits 2..7 */
#define COD_MINOR_AUDIO_HEADSET 0x04   /* Wearable Headset Device */
#define COD_MINOR_AUDIO_HANDSFREE 0x08
#define COD_MINOR_AUDIO_LOUDSPEAKER 0x14
#define COD_MINOR_AUDIO_PORTABLE_AUDIO 0x18
#define COD_MINOR_AUDIO_CAR_AUDIO 0x20
#define COD_MINOR_AUDIO_HIFI_AUDIO 0x24

bool bsp_bt_cod_is_audio(uint32_t cod)
{
    uint32_t major = cod & 0x1F00;
    uint32_t minor = cod & COD_MINOR_AUDIO_MASK;
    if (major == COD_MAJOR_AUDIO_VIDEO) {
        /* 所有 Audio/Video 大类的子类都视为音频设备 */
        return true;
    }
    (void)minor;
    return false;
}

const char *bsp_bt_get_local_addr(void)
{
    if (!s_initialized) return "";
    if (s_local_addr[0] == 0) {
        const uint8_t *bd = esp_bt_dev_get_address();
        if (bd) {
            snprintf(s_local_addr, sizeof(s_local_addr),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
        }
    }
    return s_local_addr;
}

/* ================ EIR 数据解析 (BLE 和 经典BT 共用) ================ */

/* 从 EIR (Extended Inquiry Response) / Advertising Data 解析设备名 */
static void parse_name_from_eir(uint8_t *eir, uint8_t eir_len, char *name, size_t name_cap)
{
    if (!eir || !name || name_cap == 0) return;
    name[0] = '\0';

    uint8_t offset = 0;
    while (offset < eir_len) {
        uint8_t field_len = eir[offset];
        if (field_len == 0) break;
        if (offset + field_len + 1 > eir_len) break;

        uint8_t field_type = eir[offset + 1];
        uint8_t name_len = field_len - 1;
        if (name_len >= name_cap) name_len = name_cap - 1;

        /* 0x09 = Complete Local Name, 0x08 = Shortened Local Name */
        if (field_type == 0x09 || field_type == 0x08) {
            memcpy(name, &eir[offset + 2], name_len);
            name[name_len] = '\0';
            return;
        }
        offset += field_len + 1;
    }
}

/* ================ A2DP 回调 ================ */

#if defined(CONFIG_BT_A2DP_ENABLE)
static void bt_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_connected = true;
            s_user_requested_disconnect = false;
            s_reconnect_attempts = 0;
            s_connect_fail_streak = 0;   /* 连接成功, 失败连击清零 */
            s_recovery_cycles = 0;       /* 深度恢复预算重新可用 */
            s_sdio_recovery_pending = false;
            bt_connect_watchdog_disarm();  /* 已连上, 看门狗完成使命 */
            memcpy(s_connected_bda, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            char bdstr[18];
            snprintf(bdstr, sizeof(bdstr), "%02x:%02x:%02x:%02x:%02x:%02x",
                     param->conn_stat.remote_bda[0], param->conn_stat.remote_bda[1],
                     param->conn_stat.remote_bda[2], param->conn_stat.remote_bda[3],
                     param->conn_stat.remote_bda[4], param->conn_stat.remote_bda[5]);
            strncpy(s_connected_addr, bdstr, sizeof(s_connected_addr) - 1);
            s_connected_addr[sizeof(s_connected_addr) - 1] = '\0';
            ESP_LOGI(TAG, "A2DP已连接: %s", bdstr);

            /* 先清掉断连期间播放任务积压的旧 PCM (播放任务在 BT 断开时
             * 仍会按 I2S 速率持续推流, ring 里可能残留最多 1.45s 旧音频;
             * 不清空的话重连后 SBC 会先播旧数据+静音, 真实音频被满环堵住),
             * 再预填充 64KB (~360ms) 静音: 足够 SBC 起播不 underflow,
             * 同时给真实音频留出 192KB 余量.
             * 旧版预填 256KB = ring 100% 满, 真实音频从第一块起就被
             * push_blocking 按 20ms 窗口削顶丢数据 (每块丢 ~32ms) = 卡顿. */
            bsp_bt_pcm_ring_reset();
            size_t prefilled = bsp_bt_pcm_ring_prefill(BT_PCM_PREFILL_BYTES);
            if (prefilled > 0) {
                ESP_LOGI(TAG, "Ring 预填充静音 %d 字节", (int)prefilled);
            }

            /* 尝试从已配对列表中找到名称并填充.
             *
             * 这里不能直接做 NVS 查找: bsp_bt_get_paired_by_index() 内部有
             *   bt_paired_dev_t tmp[32]; // 32 * 82 = 2624 字节
             * BTC_TASK 栈只有约 3.5KB, 本回调自身栈帧已经占了一部分, 再分配
             * 2.6KB 会触发 Stack protection fault (上一轮日志里的 SP 越界就是
             * 这里造成的). 把 NVS 查找扔给独立的工作线程, 工作线程栈 6KB
             * 足够放下. */
            {
                bt_work_msg_t msg = { .type = BT_WORK_LOOKUP_NAME };
                #pragma GCC diagnostic push
                #pragma GCC diagnostic ignored "-Wstringop-truncation"
                strncpy(msg.addr, bdstr, sizeof(msg.addr) - 1);
                #pragma GCC diagnostic pop
                msg.addr[sizeof(msg.addr) - 1] = '\0';
                if (s_bt_work_queue) {
                    (void)xQueueSend(s_bt_work_queue, &msg, 0);
                }
            }

            /* 异步读取远端名称 + 触发 A2DP START.
             *
             * 严禁在这里直接调 esp_bt_gap_read_remote_name() /
             * esp_a2d_media_ctrl(): 这俩 API 通过 btc_transfer_context()
             * 把消息发给 BTC_TASK, 而本回调正在 BTC_TASK 上下文执行 —
             * 会造成 BTC_TASK 等自己的死锁, 在 RISC-V 上会以
             * "Stack protection fault" panic 终止.
             *
             * 改为往 s_bt_work_queue 扔消息, 由独立的工作线程去调. */
            {
                bt_work_msg_t msg = { .type = BT_WORK_RMT_NAME_REQ };
                #pragma GCC diagnostic push
                #pragma GCC diagnostic ignored "-Wstringop-truncation"
                strncpy(msg.addr, bdstr, sizeof(msg.addr) - 1);
                #pragma GCC diagnostic pop
                msg.addr[sizeof(msg.addr) - 1] = '\0';
                if (s_bt_work_queue) {
                    (void)xQueueSend(s_bt_work_queue, &msg, 0);
                }
            }
            {
                bt_work_msg_t msg = { .type = BT_WORK_MEDIA_START };
                #pragma GCC diagnostic push
                #pragma GCC diagnostic ignored "-Wstringop-truncation"
                strncpy(msg.addr, bdstr, sizeof(msg.addr) - 1);
                #pragma GCC diagnostic pop
                msg.addr[sizeof(msg.addr) - 1] = '\0';
                if (s_bt_work_queue) {
                    (void)xQueueSend(s_bt_work_queue, &msg, 0);
                }
            }
            if (s_event_cb) {
                s_event_cb(true,
                           s_connected_name[0] ? s_connected_name : bdstr);
            }

            /* 关键: A2DP 已连接, 完全停止 Wi-Fi。
             * 让 SDIO 链路 100% 给 BT HCI 使用，完全消除 SDIO 争用。
             * Wi-Fi 停止不会影响 BT 音频质量，BT 可以独享全部 SDIO 带宽。 */
            bsp_wifi_set_enabled(false);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "A2DP已断开 (user_req=%d, attempts=%d)",
                     s_user_requested_disconnect, s_reconnect_attempts);
            bool user_was_disconnect = s_user_requested_disconnect;
            s_connected = false;
            s_connected_name[0] = '\0';
            bt_connect_watchdog_disarm();  /* 本次连接尝试已了结 */

            /* 丢弃 ring 里的旧 PCM: 断开期间播放任务仍按实时速率推流,
             * ring 里只剩过时数据; 不清空的话重连后先播 ~1.4s 旧音频.
             * (播放任务侧检测到断开会自动切 I2S 路径, 推入的新数据
             * 会持续覆盖/丢弃, 这里清一次保证重连时 ring 干净) */
            bsp_bt_pcm_ring_reset();

            if (s_event_cb) s_event_cb(false, NULL);

            /* BT 断开, 恢复 Wi-Fi */
            bsp_wifi_set_enabled(true);

            /* 自动重连: 仅在自动重连启用 + 非用户主动断开 + 之前已连过的设备仍可重连 */
            if (s_auto_reconnect_enabled && !user_was_disconnect &&
                s_connected_addr[0] != '\0' && s_initialized) {
                int delay_ms = s_reconnect_backoff_ms[
                    (s_reconnect_attempts < s_reconnect_backoff_count)
                        ? s_reconnect_attempts
                        : (s_reconnect_backoff_count - 1)
                ];

                /* SDIO 卡死后, 进一步推迟重连, 给 ESP-Hosted 链路充分恢复时间.
                 * 否则 HCI 命令在 SDIO 卡死期间持续触发 command_timed_out,
                 * 让链路雪上加霜. */
                if (s_sdio_recovery_pending) {
                    delay_ms = SDIO_RECOVER_COOLDOWN_MS;
                    ESP_LOGW(TAG, "SDIO 恢复冷却中, 重连延迟加大到 %dms", delay_ms);
                }

                s_reconnect_attempts++;
                ESP_LOGI(TAG, "将尝试自动重连 %s (attempts=%d, delay=%dms)",
                         s_connected_addr, s_reconnect_attempts, delay_ms);

                /* 创建一次性 timer (懒分配) */
                if (s_reconnect_timer == NULL) {
                    s_reconnect_timer = xTimerCreate("bt_reconnect",
                                                     pdMS_TO_TICKS(delay_ms),
                                                     pdFALSE, NULL,
                                                     bt_reconnect_timer_cb);
                    /* 如果首次创建时传入的 delay 不是最终值也没关系:
                     * 下次用 xTimerChangePeriod() 重设周期。 */
                }
                if (s_reconnect_timer) {
                    xTimerStop(s_reconnect_timer, 0);
                    xTimerChangePeriod(s_reconnect_timer,
                                       pdMS_TO_TICKS(delay_ms), 0);
                    xTimerStart(s_reconnect_timer, 0);
                }
            } else {
                /* 用户主动断开 -> 重置 attempt 计数 + 清空目标地址 */
                s_reconnect_attempts = 0;
                s_connected_addr[0] = '\0';
                memset(s_connected_bda, 0, sizeof(esp_bd_addr_t));
            }
        }
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START &&
            param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGI(TAG, "A2DP媒体传输已启动");
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            ESP_LOGI(TAG, "A2DP音频已开始播放");
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_SUSPEND) {
            ESP_LOGI(TAG, "A2DP音频已暂停");
        }
        break;
    default:
        break;
    }
}
#endif /* CONFIG_BT_A2DP_ENABLE */

/* ================ GAP BT 回调 (经典蓝牙发现 + A2DP) ================ */

#if defined(CONFIG_BT_CLASSIC_ENABLED)

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        /* 发现经典蓝牙设备: 解析名称、RSSI、CoD */
        ble_device_t dev = {0};
        snprintf(dev.addr, sizeof(dev.addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 param->disc_res.bda[0], param->disc_res.bda[1],
                 param->disc_res.bda[2], param->disc_res.bda[3],
                 param->disc_res.bda[4], param->disc_res.bda[5]);
        dev.dev_type = 1;  /* 经典BT */

        /* 遍历设备属性 */
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
            switch (p->type) {
            case ESP_BT_GAP_DEV_PROP_BDNAME: {
                const char *pname = (const char *)p->val;
                size_t len = strnlen(pname, p->len);
                if (len >= sizeof(dev.name)) len = sizeof(dev.name) - 1;
                memcpy(dev.name, pname, len);
                dev.name[len] = '\0';
                break;
            }
            case ESP_BT_GAP_DEV_PROP_RSSI:
                dev.rssi = *(int8_t *)p->val;
                break;
            case ESP_BT_GAP_DEV_PROP_COD: {
                uint32_t cod = *(uint32_t *)p->val;
                dev.cod = cod;
                dev.is_audio_device = bsp_bt_cod_is_audio(cod);
                break;
            }
            case ESP_BT_GAP_DEV_PROP_EIR: {
                /* 如果 BDNAME 没有提供, 从 EIR 解析名称 */
                if (dev.name[0] == '\0') {
                    parse_name_from_eir((uint8_t *)p->val, p->len,
                                        dev.name, sizeof(dev.name));
                }
                break;
            }
            default:
                break;
            }
        }

        ESP_LOGI(TAG, "发现经典BT: %s | %s | rssi=%d | cod=0x%06x %s",
                 dev.addr,
                 dev.name[0] ? dev.name : "(未知)",
                 dev.rssi, dev.cod,
                 dev.is_audio_device ? "[音频]" : "");

        if (s_classic_scan_cb) {
            s_classic_scan_cb(&dev);
        }
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(TAG, "经典蓝牙扫描开始");
        } else {
            ESP_LOGI(TAG, "经典蓝牙扫描结束");
            s_classic_scanning = false;
        }
        break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT: {
        /* 远端名称请求完成 */
        if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
            ble_device_t dev = {0};
            snprintf(dev.addr, sizeof(dev.addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                     param->read_rmt_name.bda[0], param->read_rmt_name.bda[1],
                     param->read_rmt_name.bda[2], param->read_rmt_name.bda[3],
                     param->read_rmt_name.bda[4], param->read_rmt_name.bda[5]);
            dev.dev_type = 1;
            size_t len = strnlen((const char *)param->read_rmt_name.rmt_name,
                                 sizeof(param->read_rmt_name.rmt_name));
            if (len >= sizeof(dev.name)) len = sizeof(dev.name) - 1;
            memcpy(dev.name, param->read_rmt_name.rmt_name, len);
            dev.name[len] = '\0';
            ESP_LOGI(TAG, "远端名称: %s = %s", dev.addr, dev.name);

            /* 若当前连接的就是该设备，同步更新 s_connected_name 并通知 UI */
            if (s_connected) {
                char cur_addr[18];
                const uint8_t *bda = param->read_rmt_name.bda;
                snprintf(cur_addr, sizeof(cur_addr),
                         "%02x:%02x:%02x:%02x:%02x:%02x",
                         bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
                if (strcmp(cur_addr, s_connected_addr) == 0) {
                    size_t l2 = strnlen(dev.name, sizeof(s_connected_name) - 1);
                    memcpy(s_connected_name, dev.name, l2);
                    s_connected_name[l2] = '\0';
                    if (s_event_cb) s_event_cb(true, s_connected_name);
                }
            }

            if (s_classic_scan_cb) s_classic_scan_cb(&dev);
        } else {
            ESP_LOGW(TAG, "远端名称请求失败: stat=%d", param->read_rmt_name.stat);
        }
        break;
    }
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGD(TAG, "BT 模式改变: mode=%d", param->mode_chg.mode);
        break;
    //case ESP_BT_GAP_ACL_DISCONN_CMP_EVT: {
    // case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT: {
    //     /* 链路层断连完成. reason (rsn) 含义:
    //      *   0x13 = 远端用户主动断 (最常见, 用户耳机按了关机/切换)
    //      *   0x08 = 监督超时 (链路真的丢失)
    //      *   0x14 = 远端资源不足 (耳机资源紧张也会触发)
    //      *   0x16 = 本地主机主动断 (我们自己调的)
    //      *
    //      * 我们不依赖 rsn 做业务判断, A2DP 回调会处理真正的事件.
    //      * 这里只用于 SDIO 卡死检测: 如果 ACL 断前 5s 内出现过
    //      * "eh_sdio invalid" 错误, 标记 SDIO 卡死状态, 阻止重连策略
    //      * 进一步触发更多 HCI 流量冲击 SDIO 链路. */
    //     uint8_t rsn = (uint8_t)param->acl_disconn_cmpl.reason;
    //     ESP_LOGW(TAG, "ACL 断开: rsn=0x%02x (handle=0x%x)",
    //              rsn, (unsigned)param->acl_disconn_cmpl.handle);
    //     int64_t now = esp_timer_get_time();
    //     if (now - s_last_sdio_err_us < SDIO_ERR_TIMEOUT_US) {
    //         s_sdio_recovery_pending = true;
    //         ESP_LOGE(TAG, "检测到 SDIO 卡死后才断链, 进入恢复冷却期 %dms",
    //                  SDIO_RECOVER_COOLDOWN_MS);
    //     }
    //     break;
    // }
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT: {
        /* 链路层断连完成. reason (rsn) 含义:
         *   0x13 = 远端用户主动断 (最常见, 用户耳机按了关机/切换)
         *   0x08 = 监督超时 (链路真的丢失)
         *   0x14 = 远端资源不足 (耳机资源紧张也会触发)
         *   0x16 = 本地主机主动断 (我们自己调的)
         *
         * 我们不依赖 rsn 做业务判断, A2DP 回调会处理真正的事件.
         * 这里只用于 SDIO 卡死检测: 如果 ACL 断前 5s 内出现过
         * "eh_sdio invalid" 错误, 标记 SDIO 卡死状态, 阻止重连策略
         * 进一步触发更多 HCI 流量冲击 SDIO 链路. */
        uint8_t rsn = (uint8_t)param->acl_disconn_cmpl_stat.reason;
        ESP_LOGW(TAG, "ACL 断开: rsn=0x%02x (handle=0x%x)",
                 rsn, (unsigned)param->acl_disconn_cmpl_stat.handle);
        int64_t now = esp_timer_get_time();
        if (now - s_last_sdio_err_us < SDIO_ERR_TIMEOUT_US) {
            s_sdio_recovery_pending = true;
            ESP_LOGE(TAG, "检测到 SDIO 卡死后才断链, 进入恢复冷却期 %dms",
                     SDIO_RECOVER_COOLDOWN_MS);
        }
        break;
    }
    default:
        ESP_LOGD(TAG, "未处理的 BT GAP 事件: %d", event);
        break;
    }
}
#endif /* CONFIG_BT_CLASSIC_ENABLED */

/* ================ GAP BLE 回调 (BLE扫描) ================ */

static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            if (s_ble_scan_cb) {
                ble_device_t dev = {0};
                snprintf(dev.addr, sizeof(dev.addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                         param->scan_rst.bda[0], param->scan_rst.bda[1],
                         param->scan_rst.bda[2], param->scan_rst.bda[3],
                         param->scan_rst.bda[4], param->scan_rst.bda[5]);
                dev.rssi = param->scan_rst.rssi;
                dev.dev_type = 2;  /* BLE */

                /* 尝试从Adv数据解析设备名 */
                uint8_t *adv = param->scan_rst.ble_adv;
                uint8_t adv_len = param->scan_rst.adv_data_len;
                parse_name_from_eir(adv, adv_len, dev.name, sizeof(dev.name));

                s_ble_scan_cb(&dev);
            }
        } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            s_scanning = false;
            ESP_LOGI(TAG, "BLE扫描完成");
        }
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "BLE扫描启动失败: %d", param->scan_start_cmpl.status);
            s_scanning = false;
        }
        break;
    default:
        break;
    }
}

#endif /* BT_ENABLED && BLUEDROID */

/* ================ 公共接口 ================ */

bool bsp_bt_is_initialized(void)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED)
    return s_initialized;
#else
    return false;
#endif
}

esp_err_t bsp_bt_init(void)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED)
    if (s_init_mux == NULL) {
        s_init_mux = xSemaphoreCreateMutexStatic(&s_init_mux_buf);
    }
    if (xSemaphoreTake(s_init_mux, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGE(TAG, "蓝牙初始化锁超时");
        return ESP_ERR_TIMEOUT;
    }
    if (s_initialized) {
        xSemaphoreGive(s_init_mux);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化蓝牙...");

    esp_err_t ret;
    esp_bluedroid_status_t bt_status = esp_bluedroid_get_status();
    if (bt_status == ESP_BLUEDROID_STATUS_ENABLED) {
        s_initialized = true;
        xSemaphoreGive(s_init_mux);
        ESP_LOGW(TAG, "Bluedroid 已启用，跳过重复初始化");
        return ESP_OK;
    }

    /* 安装 SDIO 卡死检测日志钩子 (一次性, 不需重复注册). */
    if (s_orig_log_vprintf == NULL) {
        s_orig_log_vprintf = esp_log_set_vprintf(sdio_log_hook);
    }

    /* 加载已配对列表 + 上次连接设备 (从 NVS) */
    load_paired_from_nvs();
    load_last_device_from_nvs();

    if (bt_status == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        ret = esp_hosted_hci_bluedroid_setup();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ESP-Hosted HCI桥接设置失败: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_init_mux);
            return ret;
        }
    } else {
        ESP_LOGW(TAG, "Bluedroid 已初始化，跳过重复 HCI 桥接");
    }

    if (bt_status == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_config_t cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
        ret = esp_bluedroid_init_with_cfg(&cfg);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Bluedroid初始化失败: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_init_mux);
            return ret;
        }
    } else {
        ESP_LOGW(TAG, "Bluedroid 已处于状态 %d, 跳过重复初始化", (int)bt_status);
    }

    bt_status = esp_bluedroid_get_status();
    if (bt_status == ESP_BLUEDROID_STATUS_INITIALIZED) {
        ret = esp_bluedroid_enable();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Bluedroid启用失败: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_init_mux);
            return ret;
        }
    } else if (bt_status != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_LOGE(TAG, "Bluedroid状态无效: %d", (int)bt_status);
        xSemaphoreGive(s_init_mux);
        return ESP_ERR_INVALID_STATE;
    }

    esp_bt_gap_set_device_name(BT_DEVICE_NAME);

#if defined(CONFIG_BT_CLASSIC_ENABLED)
    /* 注册经典 BT GAP 回调 (无论 A2DP 是否启用都需要, 用于设备发现) */
    esp_bt_gap_register_callback(bt_gap_cb);
#endif

#if defined(CONFIG_BT_A2DP_ENABLE)
    esp_a2d_register_callback(bt_a2dp_cb);
    esp_a2d_source_init();

    /* 分配 PSRAM ring buffer + 注册 PCM 提供者 (Bluedroid 会周期性回调).
     * 这条路径才是真正让 PCM 流到蓝牙耳机的方式 (ext_codec=FALSE 时
     * esp_a2d_source_audio_data_send 是 no-op). */
    if (s_pcm_ring == NULL) {
        s_pcm_ring = heap_caps_malloc(BT_PCM_RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_pcm_ring == NULL) {
            ESP_LOGW(TAG, "PCM ring PSRAM 分配失败, 改用内部 RAM");
            s_pcm_ring = heap_caps_malloc(BT_PCM_RING_SIZE, MALLOC_CAP_8BIT);
        }
        if (s_pcm_ring) {
            memset(s_pcm_ring, 0, BT_PCM_RING_SIZE);
            ESP_LOGI(TAG, "BT PCM ring buffer 已分配: %d 字节 (PSRAM/heap)", BT_PCM_RING_SIZE);
        } else {
            ESP_LOGE(TAG, "BT PCM ring buffer 分配失败");
            xSemaphoreGive(s_init_mux);
            return ESP_ERR_NO_MEM;
        }
    }
    /* 注意: 不再需要 semaphore mutex，改用 portMUX 临界区同步 */
    esp_a2d_source_register_data_callback(bt_a2dp_src_pcm_cb);
#endif

    esp_ble_gap_register_callback(ble_gap_cb);

    /* 启动 BT 延迟工作线程 (处理 A2DP 回调触发的耗时 Bluedroid API). */
    if (s_bt_work_queue == NULL) {
        s_bt_work_queue = xQueueCreate(BT_WORK_QUEUE_LEN, sizeof(bt_work_msg_t));
    }
    if (s_bt_work_queue && s_bt_work_task == NULL) {
        BaseType_t ok = pdFAIL;
        /* 优先用 PSRAM 分配工作线程栈, 大幅节省内部 RAM. */
#if (esp_idf_version >= ESP_IDF_VERSION_VAL(5, 2, 0))
        ok = xTaskCreateWithCaps(bt_work_task_main, BT_WORK_TAG,
                                 BT_WORK_TASK_STACK, NULL,
                                 BT_WORK_TASK_PRIO, &s_bt_work_task,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "PSRAM 工作线程栈分配失败, 回退到内部 RAM");
            ok = xTaskCreate(bt_work_task_main, BT_WORK_TAG,
                             BT_WORK_TASK_STACK, NULL,
                             BT_WORK_TASK_PRIO, &s_bt_work_task);
        }
#else
        ok = xTaskCreate(bt_work_task_main, BT_WORK_TAG,
                         BT_WORK_TASK_STACK, NULL,
                         BT_WORK_TASK_PRIO, &s_bt_work_task);
#endif
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "BT 工作线程创建失败");
            s_bt_work_task = NULL;
        }
    }

    s_initialized = true;
    xSemaphoreGive(s_init_mux);
    ESP_LOGI(TAG, "蓝牙初始化完成 (A2DP+BLE 4.2)");

    /* 同步已保存的音量到 BT 软件增益: s_volume 是静态硬编码默认值 (70),
     * 不加载的话, 重启后蓝牙播放永远是"初始的大声", 与设置页存储值脱节
     * (bsp_audio 侧在 app_main 更早已从 NVS 完成加载) */
    s_volume = bsp_audio_get_volume();
    ESP_LOGI(TAG, "BT 软件增益同步自已保存音量: %d", s_volume);

    /* 自动回连上次设备 (经工作线程延迟发起, 失败走标准 backoff 流程) */
    if (s_auto_connect_last && s_connected_addr[0] != '\0' && s_bt_work_queue) {
        bt_work_msg_t msg = { .type = BT_WORK_AUTO_CONNECT };
        xQueueSend(s_bt_work_queue, &msg, 0);
    }
    return ESP_OK;
#else
    ESP_LOGE("bsp_bt", "蓝牙未启用，请在menuconfig中启用BT和A2DP");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bsp_bt_a2dp_source_start(void)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_A2DP_ENABLE)
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bsp_bt_connect(const char *addr)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_A2DP_ENABLE)
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!addr) return ESP_ERR_INVALID_ARG;

    /* 取消任何 pending 的自动重连 - 用户主动 connect 优先 */
    if (s_reconnect_timer) {
        xTimerStop(s_reconnect_timer, 0);
    }
    s_reconnect_attempts = 0;
    s_user_requested_disconnect = false;

    esp_bd_addr_t bda;
    if (parse_addr_string(addr, bda) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 记下连接目标, 用作 A2DP 断开的 BDA. */
    memcpy(s_connected_bda, bda, sizeof(esp_bd_addr_t));
    strncpy(s_connected_addr, addr, sizeof(s_connected_addr) - 1);
    s_connected_addr[sizeof(s_connected_addr) - 1] = '\0';

    esp_err_t ret = esp_a2d_source_connect(bda);
    if (ret == ESP_OK) {
        /* 20s 内无任何状态事件则由看门狗把目标排回重连流程 */
        bt_connect_watchdog_arm();
    }
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bsp_bt_disconnect(void)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_A2DP_ENABLE)
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    if (s_connected) {
        /* 真实发起 A2DP 断开请求 -> Bluedroid 会异步发出
         * ESP_A2D_CONNECTION_STATE_EVT(STATE_DISCONNECTED) 事件,
         * 那时再清本地状态 (否则会立刻清标志但底层还在断). */
        s_user_requested_disconnect = true;
        s_reconnect_attempts = 0;
        bt_connect_watchdog_disarm();  /* 用户主动断开, 不再看护 */

        /* 先停掉自动重连 timer, 防止正在 disconnect 的瞬间又被重连. */
        if (s_reconnect_timer) {
            xTimerStop(s_reconnect_timer, 0);
        }

        esp_err_t ret = esp_a2d_source_disconnect(s_connected_bda);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "A2DP 断开请求失败: %s", esp_err_to_name(ret));
            /* 不立刻清标志, 等异步回调. */
            return ret;
        }
        ESP_LOGI(TAG, "已发起 A2DP 断开: %s", s_connected_addr);
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* 触发一次 SDIO 深度恢复 (通过工作线程, 重置计数后延迟重连) */
static void request_sdio_recovery(void)
{
    bt_work_msg_t msg = { .type = BT_WORK_RECOVER_SDIO };
    if (s_bt_work_queue) {
        (void)xQueueSend(s_bt_work_queue, &msg, 0);
    }
}

/* 自动重连 timer 到期回调 */
static void bt_reconnect_timer_cb(TimerHandle_t tmr)
{
    (void)tmr;
    if (!s_initialized || s_user_requested_disconnect || !s_auto_reconnect_enabled) {
        return;
    }
    if (s_connected_addr[0] == '\0') return;
    if (s_connected) {
        /* 已经连上了 (在 backoff 期间别的设备把耳机让回来了) */
        s_reconnect_attempts = 0;
        s_sdio_recovery_pending = false;  /* 连接成功, SDIO 已恢复 */
        return;
    }

    /* 经典/BLE 扫描进行中时推迟重连: Inquiry 与连接建立会争抢控制器,
     * 此时发起连接大概率失败, 白白消耗一次重连预算. 扫描结束事件不会
     * 主动重排 timer, 所以这里固定 5s 后再看. */
    if (s_classic_scanning || s_scanning) {
        ESP_LOGD(TAG, "扫描进行中, 重连推迟 5s");
        if (s_reconnect_timer) {
            xTimerStop(s_reconnect_timer, 0);
            xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(5000), 0);
            xTimerStart(s_reconnect_timer, 0);
        }
        return;
    }

    /* 达到重连上限: 先用深度恢复预算 (重置计数再试), 预算也用尽才真正
     * 停止重试, 等待用户手动连接或重启后自动回连. */
    if (s_reconnect_attempts > s_reconnect_backoff_count + 3) {
        if (s_recovery_cycles < BT_RECOVERY_CYCLES_MAX) {
            s_recovery_cycles++;
            s_reconnect_attempts = 0;
            ESP_LOGW(TAG, "自动重连达上限, 触发 SDIO 深度恢复 (第 %d/%d 轮)",
                     s_recovery_cycles, BT_RECOVERY_CYCLES_MAX);
            request_sdio_recovery();
            return;
        }
        ESP_LOGW(TAG, "自动重连已达上限且恢复预算用尽, 停止重试. 目标: %s",
                 s_connected_addr);
        return;
    }

    /* 实际连接动作 (esp_a2d_source_connect + 看门狗 + 失败重排) 移交
     * BT 工作线程执行: Tmr Svc 栈仅 2KB, 在定时器回调里直接调 Bluedroid
     * API 会栈溢出重启 (实测崩溃). 这里只做轻量检查 + 投递消息. */
    bt_work_msg_t msg = { .type = BT_WORK_RECONNECT };
    if (s_bt_work_queue == NULL ||
        xQueueSend(s_bt_work_queue, &msg, 0) != pdTRUE) {
        /* 工作队列满/线程未就绪: 推迟 2s 再投递, 不消耗重连计数 */
        ESP_LOGD(TAG, "重连投递失败, 推迟 2s");
        if (s_reconnect_timer) {
            xTimerStop(s_reconnect_timer, 0);
            xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(2000), 0);
            xTimerStart(s_reconnect_timer, 0);
        }
    }
}

/* ================ A2DP 连接请求看门狗 ================ */

/* 每次发出 esp_a2d_source_connect() 后调用: (重)启动 20s 一次性定时器 */
static void bt_connect_watchdog_arm(void)
{
    if (s_connect_watchdog == NULL) {
        s_connect_watchdog = xTimerCreate("bt_conn_wdg",
                                          pdMS_TO_TICKS(BT_CONNECT_WATCHDOG_MS),
                                          pdFALSE, NULL, bt_connect_watchdog_cb);
        if (s_connect_watchdog == NULL) {
            ESP_LOGW(TAG, "连接看门狗创建失败");
            return;
        }
    }
    xTimerStop(s_connect_watchdog, 0);
    xTimerChangePeriod(s_connect_watchdog,
                       pdMS_TO_TICKS(BT_CONNECT_WATCHDOG_MS), 0);
    xTimerStart(s_connect_watchdog, 0);
}

/* 收到 CONNECTED/DISCONNECTED 事件或用户主动断开时调用 */
static void bt_connect_watchdog_disarm(void)
{
    if (s_connect_watchdog) {
        xTimerStop(s_connect_watchdog, 0);
    }
}

/* 看门狗到期 (运行在定时器服务任务上下文):
 * 连接请求无任何状态事件 -> 按 backoff 策略重新排重连 */
static void bt_connect_watchdog_cb(TimerHandle_t tmr)
{
    (void)tmr;
    if (!s_initialized || s_connected || s_user_requested_disconnect) {
        return;
    }
    if (s_connected_addr[0] == '\0') {
        return;
    }
    /* 扫描占用控制器时连接无响应属预期, 不折腾, 等扫描结束后的
     * DISCONNECTED 事件或下一次重连. */
    if (s_classic_scanning || s_scanning) {
        ESP_LOGD(TAG, "连接看门狗: 扫描进行中, 忽略");
        return;
    }

    ESP_LOGW(TAG, "连接请求 %ds 无响应 (目标 %s), 重新排入重连流程",
             BT_CONNECT_WATCHDOG_MS / 1000, s_connected_addr);

    if (!s_auto_reconnect_enabled) {
        return;
    }

    /* 与 bt_reconnect_timer_cb 相同的深度恢复预算逻辑:
     * 重试次数耗尽 -> 先用 SDIO 深度恢复预算 */
    if (s_reconnect_attempts > s_reconnect_backoff_count + 3) {
        if (s_recovery_cycles < BT_RECOVERY_CYCLES_MAX) {
            s_recovery_cycles++;
            s_reconnect_attempts = 0;
            request_sdio_recovery();
        } else {
            ESP_LOGW(TAG, "连接看门狗: 重试与恢复预算均已耗尽, 停止. 目标: %s",
                     s_connected_addr);
        }
        return;
    }

    int idx = (s_reconnect_attempts < s_reconnect_backoff_count)
                  ? s_reconnect_attempts
                  : (s_reconnect_backoff_count - 1);
    int delay_ms = s_reconnect_backoff_ms[idx];
    if (s_sdio_recovery_pending) {
        delay_ms = SDIO_RECOVER_COOLDOWN_MS;   /* SDIO 卡死中, 加大间隔 */
    }
    s_reconnect_attempts++;

    if (s_reconnect_timer) {
        xTimerStop(s_reconnect_timer, 0);
        xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(delay_ms), 0);
        xTimerStart(s_reconnect_timer, 0);
    }
}

void bsp_bt_set_auto_reconnect(bool enable)
{
    s_auto_reconnect_enabled = enable;
    if (!enable && s_reconnect_timer) {
        xTimerStop(s_reconnect_timer, 0);
    }
    ESP_LOGI(TAG, "自动重连: %s", enable ? "启用" : "禁用");
}

void bsp_bt_set_auto_connect_last(bool enable)
{
    s_auto_connect_last = enable;
    ESP_LOGI(TAG, "开启后自动回连上次设备: %s", enable ? "启用" : "禁用");
}

/* ================ 完整反初始化 (按需启停 + 内存回收) ================ */

esp_err_t bsp_bt_deinit(void)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED)
    if (s_init_mux == NULL) return ESP_OK;
    if (xSemaphoreTake(s_init_mux, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGE(TAG, "蓝牙反初始化锁超时");
        return ESP_ERR_TIMEOUT;
    }
    if (!s_initialized) {
        xSemaphoreGive(s_init_mux);
        return ESP_OK;
    }

    /* 先关闸: 所有 API 的 s_initialized 检查从这里开始返回 INVALID_STATE */
    s_initialized = false;

    /* 1. 停止重连定时器与扫描 */
    if (s_reconnect_timer) {
        xTimerStop(s_reconnect_timer, 0);
        xTimerDelete(s_reconnect_timer, 0);
        s_reconnect_timer = NULL;
    }
    if (s_connect_watchdog) {
        xTimerStop(s_connect_watchdog, 0);
        xTimerDelete(s_connect_watchdog, 0);
        s_connect_watchdog = NULL;
    }
    if (s_scanning) {
        esp_ble_gap_stop_scanning();
        s_scanning = false;
    }
#if defined(CONFIG_BT_CLASSIC_ENABLED)
    if (s_classic_scanning) {
        esp_bt_gap_cancel_discovery();
        s_classic_scanning = false;
    }
#endif

#if defined(CONFIG_BT_A2DP_ENABLE)
    /* 2. 若已连接, 断开 A2DP 并有界等待断开事件落地.
     * A2DP 事件回调运行在 BTC_TASK, 不经过 s_init_mux, 因此持锁轮询
     * s_connected 不会阻塞事件处理. */
    if (s_connected) {
        s_user_requested_disconnect = true;
        esp_a2d_source_disconnect(s_connected_bda);
        int waited = 0;
        while (s_connected && waited < 3000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            waited += 50;
        }
        if (s_connected) {
            ESP_LOGW(TAG, "等待 A2DP 断开超时 (3s), 继续反初始化");
        }
    }

    /* 3. 反初始化 A2DP source (必须在 bluedroid disable 之前) */
    esp_a2d_source_deinit();
#endif /* CONFIG_BT_A2DP_ENABLE */

    /* 4. 关停 Bluedroid 协议栈 (释放其占用的内部 RAM) */
    esp_bluedroid_disable();
    esp_bluedroid_deinit();

    /* 5. 拆除 HCI 桥接 (内部会经 RPC 关停从机侧 BT 控制器) */
    esp_hosted_hci_bluedroid_teardown();

    /* 6. 释放 PCM ring buffer (256KB PSRAM).
     * 写侧 (audio_player) 与读侧 (SBC encoder) 此时均已停止:
     * 协议栈已关 -> 读侧不再回调; 调用方 (wless_mgr) 已在断开后
     * 留出 grace 时间, 写侧的 NULL 检查是最后一道保险. */
    portENTER_CRITICAL(&s_pcm_ring_mux);
    uint8_t *ring = s_pcm_ring;
    s_pcm_ring = NULL;
    s_pcm_ring_wp = 0;
    s_pcm_ring_rp = 0;
    s_pcm_ring_fill = 0;
    portEXIT_CRITICAL(&s_pcm_ring_mux);
    if (ring) {
        heap_caps_free(ring);
        ESP_LOGI(TAG, "PCM ring 已释放 (256KB)");
    }

    /* 7. 复位运行状态. s_connected_addr 保留 (作为回连目标), 其余清空 */
    s_connected = false;
    s_connected_name[0] = '\0';
    s_user_requested_disconnect = false;
    s_reconnect_attempts = 0;
    s_connect_fail_streak = 0;
    s_sdio_recovery_pending = false;
    s_fade_in_frames = 0;
    s_fade_in_factor = 32767;

    /* 工作线程保持存活 (栈 12KB PSRAM), 内部循环靠 s_initialized 检查
     * 跳过消息, 避免 "删队列时仍有发送者" 的竞态. */

    xSemaphoreGive(s_init_mux);
    ESP_LOGI(TAG, "蓝牙已完整反初始化, 内存已回收");
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool bsp_bt_is_connected(void)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_A2DP_ENABLE)
    return s_connected;
#else
    return false;
#endif
}

const char *bsp_bt_get_connected_name(void)
{
    if (s_connected_name[0]) return s_connected_name;
    return NULL;
}

const char *bsp_bt_get_connected_addr(void)
{
    if (s_connected_addr[0]) return s_connected_addr;
    return NULL;
}

void bsp_bt_set_event_cb(bt_event_cb_t cb)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED)
    s_event_cb = cb;
#else
    (void)cb;
#endif
}

esp_err_t bsp_bt_a2dp_send_data(const uint8_t *data, size_t len)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_A2DP_ENABLE)
    if (!s_initialized || !s_connected) return ESP_ERR_INVALID_STATE;
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    /* 把 PCM 推入 PSRAM ring buffer, 由 Bluedroid SBC encoder 异步消费.
     * 这条路径不阻塞, 不会拖慢 I2S 路径. */
    size_t written = bsp_bt_pcm_ring_push(data, len);
    return (written > 0) ? ESP_OK : ESP_FAIL;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* ================ BLE 扫描 ================ */

esp_err_t bsp_bt_scan_start(ble_scan_cb_t cb, uint32_t duration_sec)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED)
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_scanning) {
        ESP_LOGW(TAG, "BLE扫描已在进行中");
        return ESP_OK;
    }

    s_ble_scan_cb = cb;

    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,  /* 主动扫描以获取ScanRsp中的设备名 */
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE,
    };
    esp_err_t ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置BLE扫描参数失败: %s", esp_err_to_name(ret));
        return ret;
    }

    uint32_t dur = (duration_sec > 0) ? duration_sec : 5;
    ret = esp_ble_gap_start_scanning(dur);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动BLE扫描失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_scanning = true;
    ESP_LOGI(TAG, "BLE扫描开始, 持续%lu秒", dur);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void bsp_bt_scan_stop(void)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED)
    if (s_scanning) {
        esp_ble_gap_stop_scanning();
        s_scanning = false;
        ESP_LOGI(TAG, "BLE扫描停止");
    }
#endif
}

bool bsp_bt_is_scanning(void)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED)
    return s_scanning || s_classic_scanning;
#else
    return false;
#endif
}

/* ================ 经典蓝牙扫描 (用于发现蓝牙耳机/音箱) ================ */

esp_err_t bsp_bt_classic_scan_start(bt_classic_scan_cb_t cb, uint32_t duration_sec)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_CLASSIC_ENABLED)
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_classic_scanning) {
        ESP_LOGW(TAG, "经典BT扫描已在进行中");
        return ESP_OK;
    }

    s_classic_scan_cb = cb;

    /* 设置为可发现模式 (这样耳机也能反向发现我们) */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    /* 经典蓝牙查询, ESP_BT_GAP_INQ_MODE_GENERAL_INQUIRY 12.8s 等价默认时长 */
    uint32_t dur = (duration_sec > 0) ? duration_sec : 10;
    if (dur > 30) dur = 30;  /* 上限30秒, 避免长时间占用 */
    uint8_t inq_mode = ESP_BT_INQ_MODE_GENERAL_INQUIRY;
    uint8_t inq_len = (uint8_t)dur;  /* 单位: 1.28秒 */
    uint8_t num_responses = 0;        /* 0 = 不限数量 */

    esp_err_t ret = esp_bt_gap_start_discovery(inq_mode, inq_len, num_responses);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_gap_start_discovery 失败: %s", esp_err_to_name(ret));
        s_classic_scan_cb = NULL;
        return ret;
    }

    s_classic_scanning = true;
    ESP_LOGI(TAG, "经典BT扫描开始, 持续 %.2f 秒 (inquiry_len=%d)",
             inq_len * 1.28f, inq_len);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void bsp_bt_classic_scan_stop(void)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_CLASSIC_ENABLED)
    if (s_classic_scanning) {
        esp_bt_gap_cancel_discovery();
        s_classic_scanning = false;
        ESP_LOGI(TAG, "经典BT扫描停止");
    }
#endif
}

bool bsp_bt_classic_is_scanning(void)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_CLASSIC_ENABLED)
    return s_classic_scanning;
#else
    return false;
#endif
}

/* ================ 解析 "xx:xx:xx:xx:xx:xx" 到 esp_bd_addr_t ================
 *
 * ⚠️ esp_bd_addr_t = uint8_t[6]. 直接用 sscanf("%02x", &bda[i]) 会按
 *    unsigned int* 写入 4 字节, 越界破坏栈/全局变量, 触发
 *    Stack protection fault. 这里改用 unsigned int 临时变量然后逐字节
 *    memcpy, 杜绝越界写.
 */
static esp_err_t parse_addr_string(const char *addr, esp_bd_addr_t out_bda)
{
    if (!addr || !out_bda) return ESP_ERR_INVALID_ARG;
    unsigned int b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
    if (sscanf(addr, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b0, &b1, &b2, &b3, &b4, &b5) != 6) {
        return ESP_ERR_INVALID_ARG;
    }
    out_bda[0] = (uint8_t)b0;
    out_bda[1] = (uint8_t)b1;
    out_bda[2] = (uint8_t)b2;
    out_bda[3] = (uint8_t)b3;
    out_bda[4] = (uint8_t)b4;
    out_bda[5] = (uint8_t)b5;
    return ESP_OK;
}

esp_err_t bsp_bt_remote_dev_name_get(const char *addr)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_CLASSIC_ENABLED)
    if (!s_initialized || !addr) return ESP_ERR_INVALID_ARG;
    esp_bd_addr_t bda;
    if (parse_addr_string(addr, bda) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = esp_bt_gap_read_remote_name(bda);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取远端名称失败: %s", esp_err_to_name(ret));
    }
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* ================ BT 延迟工作线程 ================ */

static void bt_work_task_main(void *arg)
{
    (void)arg;
    bt_work_msg_t msg;
    ESP_LOGI(BT_WORK_TAG, "BT 工作线程启动");

    for (;;) {
        if (xQueueReceive(s_bt_work_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_initialized) {
            ESP_LOGW(BT_WORK_TAG, "蓝牙未初始化, 跳过工作消息 type=%d", (int)msg.type);
            continue;
        }

        switch (msg.type) {
        case BT_WORK_LOOKUP_NAME: {
            /* 在已配对 NVS 里查找 msg.addr 对应的设备名. 之所以不能放在
             * BTC_TASK 上跑, 是因为读取列表需要 32*82=2624B 栈, BTC_TASK
             * 栈仅 ~3.5KB. 这里工作线程栈 12KB (PSRAM) 充裕, 安全.
             * 注意: 只读一次全表再内存匹配 — 旧版对每个索引调
             * bsp_bt_get_paired_by_index(), 每次都完整重读一遍 NVS,
             * 一次连接触发 5-6 次全表读, 反复拿 flash 锁. */
            bt_paired_dev_t list[NVS_KEY_PAIRED_IDX_MAX + 1];
            int count = bsp_bt_load_paired_list(list, NVS_KEY_PAIRED_IDX_MAX + 1);
            char found_name[64] = {0};
            bool matched = false;
            for (int i = 0; i < count; i++) {
                if (strcmp(list[i].addr, msg.addr) == 0) {
                    #pragma GCC diagnostic push
                    #pragma GCC diagnostic ignored "-Wstringop-truncation"
                    strncpy(found_name, list[i].name, sizeof(found_name) - 1);
                    found_name[sizeof(found_name) - 1] = '\0';
                    #pragma GCC diagnostic pop
                    matched = true;
                    break;
                }
            }
            if (matched && s_connected) {
                /* 仅当仍然是该设备处于连接态时更新 s_connected_name 并通知 UI. */
                if (strcmp(s_connected_addr, msg.addr) == 0) {
                    strncpy(s_connected_name, found_name, sizeof(s_connected_name) - 1);
                    s_connected_name[sizeof(s_connected_name) - 1] = '\0';
                    if (s_event_cb) s_event_cb(true, s_connected_name);
                    ESP_LOGI(BT_WORK_TAG, "已配对设备名: %s -> %s",
                             msg.addr, s_connected_name);
                }
            }
            /* 持久化"上次成功连接的设备" (连接成功事件才会走到这里),
             * 供下次开启蓝牙时自动回连. 名称查到用查到的, 查不到存地址. */
            if (matched && strcmp(s_connected_addr, msg.addr) == 0) {
                save_last_device_to_nvs(msg.addr, found_name);
            } else {
                save_last_device_to_nvs(msg.addr, NULL);
            }
            break;
        }
        case BT_WORK_RMT_NAME_REQ: {
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_CLASSIC_ENABLED)
            /* 在非 BTC_TASK 上下文调用是安全的. */
            esp_bd_addr_t bda;
            if (parse_addr_string(msg.addr, bda) == ESP_OK) {
                esp_err_t r = esp_bt_gap_read_remote_name(bda);
                if (r != ESP_OK) {
                    ESP_LOGW(BT_WORK_TAG, "延迟读取远端名称失败(%s): %s",
                             msg.addr, esp_err_to_name(r));
                }
            } else {
                ESP_LOGW(BT_WORK_TAG, "延迟读取远端名称: 地址格式错误 %s", msg.addr);
            }
#endif
            break;
        }
        case BT_WORK_MEDIA_START: {
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_A2DP_ENABLE)
            /* 延迟 200ms 再启动媒体传输: ring 已预填 64KB (~360ms) 静音,
             * SBC 起播后有充足缓冲不会 underflow. 旧版 500ms 是给 256KB
             * 预填设计的, 现在预填只有 64KB, 延迟缩短让声音更快出来
             * (用户感知: 连接后播放, 开头静音从 ~0.9s 降到 ~0.5s). */
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_err_t r = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            if (r != ESP_OK) {
                ESP_LOGW(BT_WORK_TAG, "延迟 MEDIA_CTRL_START 失败: %s",
                         esp_err_to_name(r));
            } else {
                ESP_LOGI(BT_WORK_TAG, "延迟 MEDIA_CTRL_START 成功 (200ms): %s", msg.addr);
            }
#endif
            break;
        }
        case BT_WORK_RECOVER_SDIO: {
            /* SDIO 深度恢复: 冷却让 ESP-Hosted 链路自愈, 然后重置计数重连.
             * 卡死标记在位 -> 等满 30s 冷却; 否则短冷却 8s (连续同步失败
             * 升级过来的场景, 链路未必真卡死).
             * 注意: 冷却期间工作线程被占用, MEDIA_START 等消息会延后处理,
             * 属于可接受的取舍 (此时链路本来就不可用). */
            int settle_ms = s_sdio_recovery_pending ? SDIO_RECOVER_COOLDOWN_MS : 8000;
            ESP_LOGW(BT_WORK_TAG, "SDIO 深度恢复: 冷却 %dms 后重置计数并重连", settle_ms);
            vTaskDelay(pdMS_TO_TICKS(settle_ms));
            s_sdio_recovery_pending = false;
            s_connect_fail_streak = 0;
            s_reconnect_attempts = 0;
            if (s_connected_addr[0] != '\0' && s_auto_reconnect_enabled && !s_connected) {
                esp_bd_addr_t bda;
                if (parse_addr_string(s_connected_addr, bda) == ESP_OK) {
                    esp_err_t r = esp_a2d_source_connect(bda);
                    ESP_LOGI(BT_WORK_TAG, "深度恢复重连 %s: %s",
                             s_connected_addr, esp_err_to_name(r));
                    if (r == ESP_OK) {
                        bt_connect_watchdog_arm();
                    }
                }
            }
            break;
        }
        case BT_WORK_AUTO_CONNECT: {
            /* 开启蓝牙后回连上次设备: 延迟等协议栈/链路完全稳定 */
            vTaskDelay(pdMS_TO_TICKS(1500));
            if (!s_initialized || s_connected || s_user_requested_disconnect) {
                break;
            }
            if (s_connected_addr[0] == '\0' || !s_auto_connect_last) {
                break;
            }
            esp_bd_addr_t bda;
            if (parse_addr_string(s_connected_addr, bda) != ESP_OK) {
                ESP_LOGW(BT_WORK_TAG, "自动回连: 地址解析失败: %s", s_connected_addr);
                break;
            }
            ESP_LOGI(BT_WORK_TAG, "自动回连上次设备: %s", s_connected_addr);
            esp_err_t r = esp_a2d_source_connect(bda);
            if (r != ESP_OK) {
                ESP_LOGW(BT_WORK_TAG, "自动回连请求失败: %s", esp_err_to_name(r));
            } else {
                bt_connect_watchdog_arm();   /* 回连无响应时由看门狗兜底 */
            }
            break;
        }
        case BT_WORK_RECONNECT: {
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_A2DP_ENABLE)
            /* 自动重连的实体动作 (从 Tmr Svc 定时器回调迁来):
             * esp_a2d_source_connect 调用链较深, 2KB 的 Tmr Svc 栈放不下. */
            if (!s_initialized || s_user_requested_disconnect ||
                !s_auto_reconnect_enabled || s_connected ||
                s_connected_addr[0] == '\0') {
                break;
            }
            esp_bd_addr_t bda;
            if (parse_addr_string(s_connected_addr, bda) != ESP_OK) {
                ESP_LOGE(BT_WORK_TAG, "自动重连: 地址解析失败: %s", s_connected_addr);
                break;
            }
            ESP_LOGI(BT_WORK_TAG, "自动重连尝试 #%d -> %s",
                     s_reconnect_attempts, s_connected_addr);
            /* 每次发起重连前清掉 SDIO 卡死标记; 若 SDIO 实际还卡着,
             * 会在 ACL 断时再次被置位 (前面的 GAP 回调会检测). */
            s_sdio_recovery_pending = false;
            esp_err_t ret = esp_a2d_source_connect(bda);
            if (ret != ESP_OK) {
                /* Bluedroid 临时忙 (连接仍在清理中) -> 等下次 backoff 重试 */
                ESP_LOGW(BT_WORK_TAG, "自动重连请求返回 %s, 等下次 backoff 再试",
                         esp_err_to_name(ret));
                s_connect_fail_streak++;

                /* 连续同步失败: SDIO 链路大概率异常, 升级为深度恢复 */
                if (s_connect_fail_streak >= BT_CONNECT_FAIL_STREAK_MAX &&
                    s_recovery_cycles < BT_RECOVERY_CYCLES_MAX) {
                    s_recovery_cycles++;
                    s_connect_fail_streak = 0;
                    s_reconnect_attempts = 0;
                    ESP_LOGW(BT_WORK_TAG, "连续 %d 次连接失败, 触发 SDIO 深度恢复 (第 %d/%d 轮)",
                             BT_CONNECT_FAIL_STREAK_MAX, s_recovery_cycles,
                             BT_RECOVERY_CYCLES_MAX);
                    request_sdio_recovery();
                    break;
                }

                /* connect 失败后 Bluedroid 通常会发 DISCONNECTED 事件由
                 * bt_a2dp_cb 排下一次 timer; 若不发, 这里主动排下一次. */
                if (s_reconnect_timer) {
                    int next_attempts = s_reconnect_attempts + 1;
                    int next_delay = s_reconnect_backoff_ms[
                        (next_attempts < s_reconnect_backoff_count)
                            ? next_attempts
                            : (s_reconnect_backoff_count - 1)
                    ];
                    xTimerStop(s_reconnect_timer, 0);
                    xTimerChangePeriod(s_reconnect_timer,
                                       pdMS_TO_TICKS(next_delay), 0);
                    xTimerStart(s_reconnect_timer, 0);
                }
            } else {
                /* 连接请求已受理: 启动 20s 看门狗, 防止远端无响应时流程卡死 */
                bt_connect_watchdog_arm();
            }
#endif
            break;
        }
        default:
            ESP_LOGW(BT_WORK_TAG, "未知工作消息: %d", (int)msg.type);
            break;
        }
    }
}

/* ================ 配对并连接 (UI 调用入口) ================ */

esp_err_t bsp_bt_pair_and_connect(const char *addr, const char *name)
{
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_A2DP_ENABLE)
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!addr) return ESP_ERR_INVALID_ARG;

    esp_bd_addr_t bda;
    if (parse_addr_string(addr, bda) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先保存到 NVS (以便无论配对成功/失败, 重启后都能找到该设备) */
    esp_err_t save_ret = bsp_bt_save_paired(addr, name);
    if (save_ret != ESP_OK) {
        ESP_LOGW(TAG, "保存配对信息失败: %s", esp_err_to_name(save_ret));
    }

    /* 取消任何 pending 的自动重连 - 用户主动 connect 优先 */
    if (s_reconnect_timer) {
        xTimerStop(s_reconnect_timer, 0);
    }
    s_reconnect_attempts = 0;
    s_user_requested_disconnect = false;
    /* 用户主动连接也清掉 SDIO 卡死标记, 让新链路顺利建立;
     * 若 SDIO 还卡着, 1-2s 后 ACL 断事件会再次置位. */
    s_sdio_recovery_pending = false;
    memcpy(s_connected_bda, bda, sizeof(esp_bd_addr_t));
    strncpy(s_connected_addr, addr, sizeof(s_connected_addr) - 1);
    s_connected_addr[sizeof(s_connected_addr) - 1] = '\0';

    /* 触发经典蓝牙配对流程 (SSP - Just Works) 然后建立 A2DP 连接。
     * esp_a2d_source_connect 内部会自动处理首次配对。
     */
    esp_err_t ret = esp_a2d_source_connect(bda);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "发起配对/连接失败: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "已发起配对/连接: %s (name=%s)", addr, name ? name : "(unknown)");
        bt_connect_watchdog_arm();   /* 无响应时由看门狗兜底重试 */
    }
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* ================ 已配对设备持久化 (NVS) ================ */

int bsp_bt_load_paired_list(bt_paired_dev_t *out_arr, int max)
{
    if (!out_arr || max <= 0) return 0;
    if (!s_paired_mutex) load_paired_from_nvs();

    xSemaphoreTake(s_paired_mutex, portMAX_DELAY);

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_BT_PAIRED_NAMESPACE, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        s_paired_count = 0;
        xSemaphoreGive(s_paired_mutex);
        return 0;
    }

    int loaded = 0;
    int cap = (s_paired_count < max) ? s_paired_count : max;
    for (int i = 0; i < cap && i <= NVS_KEY_PAIRED_IDX_MAX; i++) {
        char key_addr[32], key_name[32];
        snprintf(key_addr, sizeof(key_addr), NVS_KEY_PAIRED_PREFIX "%d_addr", i);
        snprintf(key_name, sizeof(key_name), NVS_KEY_PAIRED_PREFIX "%d_name", i);

        out_arr[loaded].addr[0] = '\0';
        nvs_get_str_safe(h, key_addr, out_arr[loaded].addr, sizeof(out_arr[loaded].addr));

        out_arr[loaded].name[0] = '\0';
        nvs_get_str_safe(h, key_name, out_arr[loaded].name, sizeof(out_arr[loaded].name));

        if (out_arr[loaded].addr[0]) loaded++;
    }

    xSemaphoreGive(s_paired_mutex);
    nvs_close(h);

    ESP_LOGI(TAG, "读取已配对设备: %d 个", loaded);
    return loaded;
}

esp_err_t bsp_bt_save_paired(const char *addr, const char *name)
{
    if (!addr) return ESP_ERR_INVALID_ARG;

    if (!s_paired_mutex) {
        s_paired_mutex = xSemaphoreCreateMutex();
    }
    if (!s_paired_mutex) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_paired_mutex, portMAX_DELAY);

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_BT_PAIRED_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_paired_mutex);
        ESP_LOGE(TAG, "打开NVS失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 1) 先读取所有现有记录到内存 */
    bt_paired_dev_t list[NVS_KEY_PAIRED_IDX_MAX + 1];
    memset(list, 0, sizeof(list));
    int existing = 0;
    uint8_t cnt_u8 = 0;
    if (nvs_get_u8(h, NVS_KEY_PAIRED_COUNT, &cnt_u8) == ESP_OK) {
        existing = cnt_u8;
    }
    if (existing > NVS_KEY_PAIRED_IDX_MAX + 1) existing = NVS_KEY_PAIRED_IDX_MAX + 1;

    for (int i = 0; i < existing; i++) {
        char ka[32], kn[32];
        snprintf(ka, sizeof(ka), NVS_KEY_PAIRED_PREFIX "%d_addr", i);
        snprintf(kn, sizeof(kn), NVS_KEY_PAIRED_PREFIX "%d_name", i);
        nvs_get_str_safe(h, ka, list[i].addr, sizeof(list[i].addr));
        nvs_get_str_safe(h, kn, list[i].name, sizeof(list[i].name));
    }

    /* 2) 查找是否已存在该地址 -> 更新名称 */
    int update_idx = -1;
    for (int i = 0; i < existing; i++) {
        if (strcmp(list[i].addr, addr) == 0) { update_idx = i; break; }
    }

    if (update_idx >= 0) {
        if (name && name[0]) {
            strncpy(list[update_idx].name, name, sizeof(list[update_idx].name) - 1);
            list[update_idx].name[sizeof(list[update_idx].name) - 1] = '\0';
        }
    } else {
        /* 添加到末尾 */
        if (existing >= NVS_KEY_PAIRED_IDX_MAX + 1) {
            /* 已满 -> 删除最早 (第0条), 整体前移 */
            for (int i = 1; i < existing; i++) list[i - 1] = list[i];
            existing--;
        }
        strncpy(list[existing].addr, addr, sizeof(list[existing].addr) - 1);
        list[existing].addr[sizeof(list[existing].addr) - 1] = '\0';
        if (name && name[0]) {
            strncpy(list[existing].name, name, sizeof(list[existing].name) - 1);
            list[existing].name[sizeof(list[existing].name) - 1] = '\0';
        } else {
            strncpy(list[existing].name, addr, sizeof(list[existing].name) - 1);
            list[existing].name[sizeof(list[existing].name) - 1] = '\0';
        }
        existing++;
    }

    /* 3) 清除旧记录 */
    for (int i = 0; i <= NVS_KEY_PAIRED_IDX_MAX; i++) {
        char ka[32], kn[32];
        snprintf(ka, sizeof(ka), NVS_KEY_PAIRED_PREFIX "%d_addr", i);
        snprintf(kn, sizeof(kn), NVS_KEY_PAIRED_PREFIX "%d_name", i);
        nvs_erase_key(h, ka);
        nvs_erase_key(h, kn);
    }

    /* 4) 回写 */
    for (int i = 0; i < existing; i++) {
        char ka[32], kn[32];
        snprintf(ka, sizeof(ka), NVS_KEY_PAIRED_PREFIX "%d_addr", i);
        snprintf(kn, sizeof(kn), NVS_KEY_PAIRED_PREFIX "%d_name", i);
        nvs_set_str(h, ka, list[i].addr);
        nvs_set_str(h, kn, list[i].name);
    }
    nvs_set_u8(h, NVS_KEY_PAIRED_COUNT, (uint8_t)existing);
    ret = nvs_commit(h);
    nvs_close(h);

    s_paired_count = existing;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "已配对设备已保存: %s (%s), 共 %d 条",
                 addr, name ? name : "(unknown)", existing);
    }
    xSemaphoreGive(s_paired_mutex);
    return ret;
}

esp_err_t bsp_bt_remove_paired(const char *addr)
{
    if (!addr) return ESP_ERR_INVALID_ARG;

    if (!s_paired_mutex) {
        s_paired_mutex = xSemaphoreCreateMutex();
    }
    if (!s_paired_mutex) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_paired_mutex, portMAX_DELAY);

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_BT_PAIRED_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_paired_mutex);
        return ret;
    }

    /* 加载全部 */
    bt_paired_dev_t list[NVS_KEY_PAIRED_IDX_MAX + 1];
    memset(list, 0, sizeof(list));
    int existing = 0;
    uint8_t cnt_u8 = 0;
    if (nvs_get_u8(h, NVS_KEY_PAIRED_COUNT, &cnt_u8) == ESP_OK) existing = cnt_u8;
    if (existing > NVS_KEY_PAIRED_IDX_MAX + 1) existing = NVS_KEY_PAIRED_IDX_MAX + 1;

    for (int i = 0; i < existing; i++) {
        char ka[32], kn[32];
        snprintf(ka, sizeof(ka), NVS_KEY_PAIRED_PREFIX "%d_addr", i);
        snprintf(kn, sizeof(kn), NVS_KEY_PAIRED_PREFIX "%d_name", i);
        nvs_get_str_safe(h, ka, list[i].addr, sizeof(list[i].addr));
        nvs_get_str_safe(h, kn, list[i].name, sizeof(list[i].name));
    }

    /* 移除匹配项 */
    int removed = 0;
    int write_idx = 0;
    for (int i = 0; i < existing; i++) {
        if (strcmp(list[i].addr, addr) == 0) {
            removed = 1;
            continue;
        }
        if (write_idx != i) list[write_idx] = list[i];
        write_idx++;
    }
    int new_count = write_idx;

    /* 清除 + 重写 */
    for (int i = 0; i <= NVS_KEY_PAIRED_IDX_MAX; i++) {
        char ka[32], kn[32];
        snprintf(ka, sizeof(ka), NVS_KEY_PAIRED_PREFIX "%d_addr", i);
        snprintf(kn, sizeof(kn), NVS_KEY_PAIRED_PREFIX "%d_name", i);
        nvs_erase_key(h, ka);
        nvs_erase_key(h, kn);
    }
    for (int i = 0; i < new_count; i++) {
        char ka[32], kn[32];
        snprintf(ka, sizeof(ka), NVS_KEY_PAIRED_PREFIX "%d_addr", i);
        snprintf(kn, sizeof(kn), NVS_KEY_PAIRED_PREFIX "%d_name", i);
        nvs_set_str(h, ka, list[i].addr);
        nvs_set_str(h, kn, list[i].name);
    }
    nvs_set_u8(h, NVS_KEY_PAIRED_COUNT, (uint8_t)new_count);
    ret = nvs_commit(h);
    nvs_close(h);

    s_paired_count = new_count;

    /* 如蓝牙已初始化, 同步移除 Bluedroid 层的绑定记录 */
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && defined(CONFIG_BT_CLASSIC_ENABLED)
    if (s_initialized && removed) {
        esp_bd_addr_t bda;
        if (parse_addr_string(addr, bda) == ESP_OK) {
            esp_bt_gap_remove_bond_device(bda);
        }
    }
#endif

    xSemaphoreGive(s_paired_mutex);
    ESP_LOGI(TAG, "已删除配对设备: %s (命中=%d)", addr, removed);
    return ret;
}

int bsp_bt_get_paired_count(void)
{
    return s_paired_count;
}

bool bsp_bt_get_paired_by_index(int index, bt_paired_dev_t *out)
{
    if (index < 0 || index >= s_paired_count || !out) return false;

    bt_paired_dev_t tmp[NVS_KEY_PAIRED_IDX_MAX + 1];
    int n = bsp_bt_load_paired_list(tmp, NVS_KEY_PAIRED_IDX_MAX + 1);
    if (index < n) {
        *out = tmp[index];
        return true;
    }
    return false;
}
