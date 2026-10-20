/**
 * @file audio_player.c
 * @brief 音频播放器实现 - 支持 WAV/MP3 格式 + 记忆播放 + 播放模式 + ID3/歌词/收藏/睡眠
 *
 * 功能:
 *   - 扫描 SD 卡 /sdcard/music/ 目录下的 .wav 和 .mp3 文件
 *   - 通过统一解码器接口播放 WAV 和 MP3
 *   - 播放控制: 播放、暂停、恢复、停止、上一曲、下一曲、跳转
 *   - 播放模式: 顺序 / 随机 / 单曲循环 / 列表循环 (持久化)
 *   - ID3 元数据 (标题/歌手), LRC 歌词解析, 收藏夹, 睡眠定时
 *   - 记忆播放: 保存/恢复播放进度 (索引+秒数+模式), 断电续播
 *   - 独立 FreeRTOS 播放任务 (事件驱动循环, 无递归)
 *   - 进度回调通知 UI
 *   - PCM缓冲区使用PSRAM减少内部内存占用
 */

#include "audio_player.h"
#include "audio_decoder.h"
#include "bsp_audio.h"
#include "bsp_board.h"
#include "bsp_sd_card.h"
#include "bsp_bt.h"
#include "bsp_brightness.h"
#include "id3_tags.h"
#include "lrc_parser.h"
#include "fft_spectrum.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp_memory.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if (esp_idf_version >= ESP_IDF_VERSION_VAL(5, 2, 0))
#include "freertos/idf_additions.h"
#endif

static const char *TAG = "audio_player";

/* ================ 常量定义 ================ */

#define MAX_MUSIC_COUNT     256     /* 最多支持音乐数量 (列表全在 PSRAM, 256*128B=32KB) */
#define MAX_MUSIC_NAME_LEN  128     /* 文件名最大长度 */
#define READ_BUF_SIZE       16384   /* 读取/PCM 缓冲区 16KB (约 93ms @ 44.1k stereo) */
/* FreeRTOS 的 usStackDepth 单位是 StackType_t 个数，不是字节。
 * ESP32-P4 上 StackType_t 为 4 字节，12KB 栈应传 3072，而不是 12288。 */
#define PLAYER_TASK_STACK_BYTES  (12 * 1024)
#define PLAYER_TASK_STACK        (PLAYER_TASK_STACK_BYTES / sizeof(StackType_t))
#define PLAYER_TASK_PRIO    5       /* 播放任务优先级 */
/* 进度保存间隔(秒)。保存在 NVS (约1-5ms), 但仍不宜过频 (NVS 擦写寿命);
 * 旧版 5s 写 SD 卡一次, SD 写延迟可达 50-200ms, 会造成周期性播放卡顿 */
#define PROGRESS_SAVE_INTERVAL  30

/* ================ PCM 时钟节流 (保证播放速度正确) ================ */
/*
 * 问题背景:
 *   走 I2S 时, i2s_channel_write(portMAX_DELAY) 会自然按 I2S DMA 速率节流.
 *   走 BT 时 (推 PCM 到 bsp_bt ring buffer), SBC encoder 按 44.1/48kHz 异步
 *   拉数据; 若 audio_player_task 解码速度远快于 SBC 消费速度, ring 会
 *   涨满, 等 SBC 慢慢消费完才会推进到下一帧 -> 用户感受到"音乐速度明显
 *   加快" (实际是 player_task 在跑快, 但 SBC 还是按真采样率消费, 大量
 *   PCM 在 ring 中等被消费完后, UI 进度条飞跑).
 *
 * 解决方案:
 *   用"绝对时基"给 PCM 输出端做节流: 每次解码出 N 字节 PCM 后, 算出对应
 *   的播放时长 t_ms = N * 1000 / (sample_rate * 2ch * 2bytes). 等到
 *   "上一帧时间戳 + t_ms"才继续解码. 若跑得不够快则不阻塞 (I2S 路径
 *   自然阻塞, BT 路径不会卡).
 */
typedef struct {
    int64_t  t0_us;             /* 第一帧解码起点 (esp_timer_get_time) */
    int64_t  next_frame_us;     /* 下一帧应当被推完的目标时间 (绝对) */
    uint32_t total_pcm_frames;  /* 累计已送出的 stereo 帧数 (用于计算进度) */
} pcm_clock_t;

static pcm_clock_t s_clock = { 0 };
static void *player_malloc(size_t size);

/* 安全定长字符串复制 (始终置 '\0', 不触发 strncpy 截断警告) */
static void copy_fixed(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (src) {
        size_t n = strnlen(src, cap - 1);
        memcpy(dst, src, n);
        dst[n] = '\0';
    } else {
        dst[0] = '\0';
    }
}

/* 记忆播放: 主存储 NVS (namespace "player"); 旧版 SD 卡 playback.cfg 仅作
 * 一次性迁移读取。SD 写在播放任务里做会引入 50-200ms 的周期性卡顿,
 * NVS 写仅 1-5ms 且与 SD 总线无关。 */
#define PLAYBACK_NVS_NS     "player"
#define PLAYBACK_FILE       "/sdcard/playback.cfg"   /* 旧版文件, 只读迁移 */
#define FAVORITES_FILE      "/sdcard/favorites.cfg"
static const int16_t silence_pcm[256] = {0};

/* SRC 输出缓冲 (PSRAM, 8KB) */
static int16_t *s_src_out_buf = NULL;

/* ================ 48kHz->44.1kHz 采样率转换 (SBC编码器固定44.1kHz) ================
 *
 * ESP-IDF Bluedroid 的 SBC 编码器内部硬编码为 44.1kHz，无法配置。
 * 而大多数 MP3 文件是 48kHz 采样率，直接送入会导致播放速度变慢 (48/44.1 ≈ 1.09x)。
 * 解决方案: 软件线性插值重采样 (48000/44100 = 160/147)。
 */
typedef struct {
    int16_t *input_buf;     /* 输入缓冲区 (stereo int16, 帧数 x 2) */
    int input_capacity;     /* 输入缓冲区总帧数容量 */
    int input_size;         /* 当前有效输入帧数 */
    int64_t phase;          /* 相位累加器 (固定点 12.20) */
} src48_to_441_t;

static src48_to_441_t s_src = {0};

#define SRC_PHASE_SHIFT  20
#define SRC_IN_STEPS     160   /* 输入步进: 48kHz */
#define SRC_OUT_STEPS    147   /* 输出步进: 44.1kHz */
#define SRC_FRAC_MASK    ((1 << SRC_PHASE_SHIFT) - 1)

/* 初始化采样率转换器 */
static bool src_init(int max_input_frames)
{
    if (s_src.input_buf) free(s_src.input_buf);

    s_src.input_capacity = max_input_frames + SRC_IN_STEPS;
    s_src.input_buf = player_malloc(s_src.input_capacity * 4);  /* stereo: x2 int16 */
    if (!s_src.input_buf) {
        s_src.input_capacity = 0;
        return false;
    }

    s_src.input_size = 0;
    s_src.phase = 0;
    return true;
}

/* 清理采样率转换器 */
static void src_deinit(void)
{
    if (s_src.input_buf) { free(s_src.input_buf); s_src.input_buf = NULL; }
    s_src.input_size = 0;
    s_src.input_capacity = 0;
}

/* 送入原始 PCM 数据 (stereo int16) */
static void src_feed(const int16_t *pcm_frames, int frame_count)
{
    if (!s_src.input_buf || frame_count <= 0) return;

    if (s_src.input_size > 0 && frame_count > 0) {
        int keep = (s_src.input_size > SRC_IN_STEPS) ? SRC_IN_STEPS : s_src.input_size;
        if (keep > 0) {
            memmove(s_src.input_buf, s_src.input_buf + (s_src.input_size - keep) * 2, keep * 4);
        }
        s_src.input_size = keep;
    }

    int free_space = s_src.input_capacity - s_src.input_size;
    int copy = (frame_count < free_space) ? frame_count : free_space;
    if (copy > 0) {
        memcpy(s_src.input_buf + s_src.input_size * 2, pcm_frames, copy * 4);
        s_src.input_size += copy;
    }
}

/* 取出转换后的 PCM 数据 (stereo int16) */
static int src_read(int16_t *out_frames, int max_out_frames)
{
    if (!s_src.input_buf || max_out_frames <= 0) return 0;

    int out_count = 0;

    while (out_count < max_out_frames) {
        if (s_src.input_size < 2) {
            break;
        }

        int64_t in_pos_fixed = s_src.phase >> SRC_PHASE_SHIFT;
        int in_idx = (int)in_pos_fixed;

        if (in_idx >= s_src.input_size - 1) {
            break;
        }

        int64_t frac = s_src.phase & SRC_FRAC_MASK;
        int64_t w1 = frac;
        int64_t w0 = SRC_FRAC_MASK - w1;

        int idx0 = in_idx;
        int idx1 = idx0 + 1;

        int64_t s0_l = s_src.input_buf[idx0 * 2 + 0];
        int64_t s1_l = s_src.input_buf[idx1 * 2 + 0];
        int64_t out_l = (s0_l * w0 + s1_l * w1 + (1 << (SRC_PHASE_SHIFT - 1))) >> SRC_PHASE_SHIFT;
        out_l = (out_l > 32767) ? 32767 : (out_l < -32768) ? -32768 : out_l;
        out_frames[out_count * 2 + 0] = (int16_t)out_l;

        int64_t s0_r = s_src.input_buf[idx0 * 2 + 1];
        int64_t s1_r = s_src.input_buf[idx1 * 2 + 1];
        int64_t out_r = (s0_r * w0 + s1_r * w1 + (1 << (SRC_PHASE_SHIFT - 1))) >> SRC_PHASE_SHIFT;
        out_r = (out_r > 32767) ? 32767 : (out_r < -32768) ? -32768 : out_r;
        out_frames[out_count * 2 + 1] = (int16_t)out_r;

        out_count++;

        s_src.phase += (int64_t)SRC_OUT_STEPS << SRC_PHASE_SHIFT;

        while (s_src.phase >= ((int64_t)s_src.input_size << SRC_PHASE_SHIFT) && s_src.input_size > 0) {
            s_src.input_size--;
            s_src.phase -= (int64_t)1 << SRC_PHASE_SHIFT;
            if (s_src.input_size > 0) {
                memmove(s_src.input_buf, s_src.input_buf + 2, s_src.input_size * 4);
            }
        }
    }

    return out_count;
}

/* ================ 静态变量 ================ */

/* 音乐列表 - 64 * 128 = 8KB, 全部迁到 PSRAM 以节省内部 RAM. */
static char (*s_music_list)[MAX_MUSIC_NAME_LEN] = NULL;
static int  s_music_count = 0;

/* ID3 元数据 (PSRAM): 标题 64*128, 歌手 64*64 */
static char (*s_title_list)[128]  = NULL;
static char (*s_artist_list)[64]  = NULL;

/* LRC 歌词 (PSRAM, init 时分配) */
static lrc_line_t *s_lyrics = NULL;
static int  s_lyrics_count = 0;

/* 收藏夹 (是否收藏, 索引对齐 s_music_list) */
static bool s_fav[MAX_MUSIC_COUNT];

/* 播放状态 */
static volatile audio_state_t s_state = AUDIO_STATE_IDLE;
static volatile int s_current_index = -1;

/* 播放模式 (持久化) */
static play_mode_t s_play_mode = PLAY_MODE_REPEAT_ALL;

/* 控制标志 */
static volatile bool s_cmd_stop  = false;
static volatile bool s_cmd_pause = false;
static volatile bool s_cmd_seek  = false;
static volatile uint32_t s_seek_target = 0;  /* seek目标位置(秒) */
/* 任务退出请求: true = 内层循环结束后任务直接退出;
 * false = s_cmd_stop 仅表示"切歌", 外层循环继续采用新的 s_current_index */
static volatile bool s_task_should_exit = false;

/* 播放任务句柄 */
static TaskHandle_t s_player_task_handle = NULL;

/* BT 淡入状态 */
static bool s_fade_in_started = false;

/* 睡眠定时: 到期毫秒 (esp_timer_get_time/1000), 0 = 未启用 */
static volatile uint64_t s_sleep_until_ms = 0;
static bool s_sleep_off = false;          /* 睡眠定时触发后已熄屏 */
static uint8_t s_brightness_saved = 0;    /* 熄屏前的亮度 */

/* 进度回调 */
static void (*s_progress_cb)(int index, uint32_t cur, uint32_t total, const char *title) = NULL;
/* 曲目开始回调 */
static void (*s_track_cb)(int index) = NULL;
/* 播放状态变化回调 (通知 UI 进入/离开 BUFFERING 等状态) */
static void (*s_state_cb)(audio_state_t state) = NULL;

/* 后台扫描 (开机用): UI 在扫描完成后收播放列表 */
static void (*s_scan_done_cb)(int count) = NULL;
static volatile bool s_scan_busy = false;

/**
 * @brief 统一的状态更新入口: 写 s_state 并通知 UI 回调
 *        (回调运行在调用者线程, UI 侧需自行转到 LVGL 线程)
 */
static void set_state(audio_state_t st)
{
    s_state = st;
    if (s_state_cb) s_state_cb(st);
}

/* 播放进度保存 */
static volatile uint32_t s_last_saved_sec = 0;

/* ================ 记忆播放: 保存/加载 ================ */

/* 保存指定曲目的播放进度到 NVS (任务切歌时索引已被更新, 必须显式传旧曲信息) */
static void save_progress_at(int index, uint32_t seconds)
{
    if (index < 0) return;

    nvs_handle_t h;
    if (nvs_open(PLAYBACK_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, "idx", (int32_t)index);
    nvs_set_u32(h, "pos", seconds);
    nvs_set_i32(h, "mode", (int32_t)s_play_mode);
    nvs_commit(h);
    nvs_close(h);
}

/* 保存播放进度到SD卡 */
void audio_player_save_progress(void)
{
    save_progress_at(s_current_index, s_last_saved_sec);
}

/* 旧版 SD 卡 playback.cfg 读取 (一次性迁移): 索引(4)+秒数(4)+模式(4) */
static bool load_progress_legacy(int32_t *index, uint32_t *seconds, int32_t *mode)
{
    FILE *f = fopen(PLAYBACK_FILE, "rb");
    if (!f) return false;

    int32_t idx = -1;
    uint32_t pos = 0;
    bool ok = false;
    if (fread(&idx, sizeof(idx), 1, f) == 1 &&
        fread(&pos, sizeof(pos), 1, f) == 1) {
        ok = true;
        int32_t m = -1;
        if (fread(&m, sizeof(m), 1, f) == 1) {
            *mode = m;
        }
    }
    fclose(f);
    if (ok) {
        *index = idx;
        *seconds = pos;
    }
    return ok;
}

/* 从 NVS 加载播放进度; NVS 无记录时尝试旧版 SD 文件并迁移到 NVS */
bool audio_player_load_progress(int *index, uint32_t *seconds)
{
    nvs_handle_t h;
    if (nvs_open(PLAYBACK_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t idx = -1;
        uint32_t pos = 0;
        int32_t mode = -1;
        bool ok = (nvs_get_i32(h, "idx", &idx) == ESP_OK &&
                   nvs_get_u32(h, "pos", &pos) == ESP_OK);
        /* 模式可选 (兼容只写了 idx/pos 的情况) */
        if (nvs_get_i32(h, "mode", &mode) != ESP_OK) mode = -1;
        nvs_close(h);

        if (ok && idx >= 0 && idx < s_music_count) {
            if (index)   *index = idx;
            if (seconds) *seconds = pos;
            if (mode >= PLAY_MODE_SEQUENCE && mode <= PLAY_MODE_REPEAT_ALL) {
                s_play_mode = (play_mode_t)mode;
            }
            ESP_LOGI(TAG, "加载播放进度(NVS): 索引=%d, 位置=%lus, 模式=%d",
                     idx, pos, (int)s_play_mode);
            return true;
        }
        if (ok) {
            return false;   /* NVS 有记录但索引越界 (列表变了), 不回退旧文件 */
        }
    }

    /* NVS 无记录: 尝试旧版 SD 文件并迁移 */
    int32_t mode = -1;
    int32_t idx = -1;
    uint32_t pos = 0;
    if (load_progress_legacy(&idx, &pos, &mode)) {
        if (idx >= 0 && idx < s_music_count) {
            if (index)   *index = idx;
            if (seconds) *seconds = pos;
            if (mode >= PLAY_MODE_SEQUENCE && mode <= PLAY_MODE_REPEAT_ALL) {
                s_play_mode = (play_mode_t)mode;
            }
            save_progress_at(idx, pos);   /* 迁移到 NVS */
            ESP_LOGI(TAG, "从旧版playback.cfg迁移进度: 索引=%d, 位置=%lus", idx, pos);
            return true;
        }
    }
    return false;
}

/* 跳转到指定位置 */
void audio_player_seek(uint32_t seconds)
{
    if (s_state != AUDIO_STATE_PLAYING && s_state != AUDIO_STATE_PAUSED) {
        return;
    }
    s_seek_target = seconds;
    s_cmd_seek = true;
}

/* ================ 内部函数 ================ */

/**
 * @brief 分配音频缓冲区（内部 SRAM）
 */
static void *player_malloc(size_t size)
{
    /* 音频大缓冲优先使用 PSRAM，内部 RAM 只作为回退，避免挤占
     * I2S/LVGL/FreeRTOS 所需的 DMA 和任务内存。 */
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ESP_LOGW(TAG, "PSRAM不足，尝试内部RAM分配 %d 字节", (int)size);
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

/**
 * @brief 根据播放模式计算下一曲索引
 * @return -1 表示顺序模式播完, 应停止
 */
static int next_track_index(play_mode_t mode, int cur)
{
    int n = s_music_count;
    if (n <= 0) return -1;
    if (cur < 0 || cur >= n) cur = 0;

    switch (mode) {
    case PLAY_MODE_REPEAT_ONE:
        return cur;                       /* 单曲循环 */
    case PLAY_MODE_SHUFFLE:
        return (int)(esp_random() % (uint32_t)n);   /* 随机 */
    case PLAY_MODE_REPEAT_ALL:
        return (cur + 1) % n;             /* 列表循环 */
    case PLAY_MODE_SEQUENCE:
    default:
        return (cur + 1 < n) ? (cur + 1) : -1;      /* 顺序, 播完停止 */
    }
}

/**
 * @brief 恢复熄屏亮度 (睡眠定时或手动暂停导致)
 */
static void restore_screen_brightness(void)
{
    if (s_sleep_off) {
        bsp_brightness_set(s_brightness_saved ? s_brightness_saved : BRIGHTNESS_DEFAULT);
        s_sleep_off = false;
        ESP_LOGI(TAG, "恢复屏幕亮度: %d", bsp_brightness_get());
    }
}

/**
 * @brief 播放任务主函数 (事件驱动循环, 无递归)
 *
 * 外层循环负责曲目切换 (自动下一曲), 内层循环负责单曲播放.
 * 手动切歌 (next/prev/play_index) 通过 s_cmd_stop + 新任务完成,
 * 不会出现栈累积.
 */
/* 播放任务 PCM 缓冲: 常驻 (32KB PSRAM), 避免每次任务重建 alloc/free 造成碎片 */
static int16_t *s_pcm_task_buf = NULL;

static void audio_player_task(void *arg)
{
    int16_t *pcm_buf = s_pcm_task_buf;
    if (pcm_buf == NULL) {
        pcm_buf = player_malloc(READ_BUF_SIZE * 2);
        if (pcm_buf != NULL) {
            s_pcm_task_buf = pcm_buf;
        }
    }
    if (pcm_buf == NULL) {
        ESP_LOGE(TAG, "分配PCM缓冲区失败");
        set_state(AUDIO_STATE_STOPPED);
        s_player_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* 第一曲可恢复记忆进度; 自动切歌不再恢复 */
    bool first_track = true;

    while (s_current_index >= 0 && s_current_index < s_music_count) {
        /* ================== 单曲播放段 ================== */
        int index = s_current_index;

        /* 重置控制标志 */
        s_cmd_stop  = false;
        s_cmd_pause = false;
        s_cmd_seek  = false;
        s_fade_in_started = false;
        s_last_saved_sec = 0;

        /* 进入"准备中"状态: 解码器打开 (SD读取/MP3头解析) 期间 UI 显示等待 */
        set_state(AUDIO_STATE_BUFFERING);

        /* 构建完整文件路径 */
        char filepath[256];
        snprintf(filepath, sizeof(filepath), "%s/%s", MUSIC_DIR_PATH, s_music_list[index]);

        /* 打开解码器 */
        audio_info_t info;
        if (!audio_decoder_open(filepath, &info)) {
            ESP_LOGE(TAG, "解码器打开失败: %s (自动跳过)", filepath);
            set_state(AUDIO_STATE_STOPPED);
            /* 打开期间来了停止/切歌请求: 优先响应, 不做自动跳过 */
            if (s_cmd_stop) {
                if (s_task_should_exit) {
                    break;
                }
                continue;
            }
            /* 跳过损坏文件 (单曲循环下避免死循环) */
            int next = (s_music_count > 1) ? (index + 1) % s_music_count : -1;
            if (next < 0) break;
            s_current_index = next;
            first_track = false;
            continue;
        }

        /* 通知 UI 曲目开始 */
        if (s_track_cb) s_track_cb(index);

        /* I2S 始终配置为立体声输出 (CS43131 也是立体声 DAC) */
        const int output_channels = 2;
        const int output_bytes_per_frame = output_channels * 2;  /* 4 字节/帧 */

        /* BT 路径采样率转换: 48kHz -> 44.1kHz (SBC 固定 44.1kHz) */
        const bool need_src = (info.sample_rate == 48000);
        const uint32_t bt_sample_rate = need_src ? 44100 : info.sample_rate;
        if (need_src) {
            ESP_LOGI(TAG, "SRC: 启用 48kHz->44.1kHz 转换");
            src_init(READ_BUF_SIZE / 4);
        }

        /* 设置采样率 (I2S 会自动切换 CS43131 寄存器) */
        if (info.sample_rate > 0) {
            bsp_audio_set_sample_rate(info.sample_rate);
        }

        /* 解码器已就绪, 采样率已配置 -> 正式进入播放状态 (UI 收起等待指示) */
        set_state(AUDIO_STATE_PLAYING);

        /* 恢复屏幕 (睡眠熄屏后用户手动播放) */
        restore_screen_brightness();

        /* 检查是否有保存的进度需要恢复 (仅任务的第一曲) */
        if (first_track) {
            int saved_idx;
            uint32_t saved_pos;
            if (audio_player_load_progress(&saved_idx, &saved_pos)) {
                if (saved_idx == index && saved_pos > 2 && saved_pos < info.total_seconds) {
                    ESP_LOGI(TAG, "恢复播放进度: %lus", saved_pos);
                    audio_decoder_seek(saved_pos);
                    s_last_saved_sec = saved_pos;
                } else if (saved_idx == index && info.total_seconds > 0 &&
                           saved_pos >= info.total_seconds) {
                    /* 上次已播完整曲 (保存位置>=时长): 清零进度, 本次从头播放 */
                    ESP_LOGI(TAG, "上次已播完, 进度清零");
                    save_progress_at(index, 0);
                    s_last_saved_sec = 0;
                }
            }
        }
        first_track = false;

        /* 初始化 PCM 时钟节流 */
        {
            s_clock.t0_us          = esp_timer_get_time();
            s_clock.next_frame_us  = s_clock.t0_us;
            s_clock.total_pcm_frames = 0;
        }

        /* BT 播放诊断 + 进度去重 (节流时基在 s_clock.next_frame_us) */
        uint32_t last_diag_tick = 0;
        uint32_t last_progress_sec = 0xFFFFFFFF;

        /* ================== 单曲播放循环 ================== */
        while (!s_cmd_stop) {
            /* 睡眠定时检查 */
            if (s_sleep_until_ms != 0) {
                uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
                if (now_ms >= s_sleep_until_ms) {
                    ESP_LOGI(TAG, "睡眠定时已到, 暂停播放并熄屏");
                    s_sleep_until_ms = 0;
                    if (!s_sleep_off) {
                        s_brightness_saved = bsp_brightness_get();
                        bsp_brightness_set_raw(0);
                        s_sleep_off = true;
                    }
                    s_cmd_pause = true;
                }
            }

            /* 检查暂停 */
            if (s_cmd_pause) {
                set_state(AUDIO_STATE_PAUSED);
                /* 暂停即兜底保存进度 (30s 周期保存之外的同步点) */
                audio_player_save_progress();
                /* 暂停时: 时钟基对齐，并给 BT ring 填充静音 PCM 防止 underflow. */
                s_clock.next_frame_us = esp_timer_get_time();
                while (s_cmd_pause && !s_cmd_stop) {
                    bsp_bt_pcm_ring_push((const uint8_t *)silence_pcm, sizeof(silence_pcm));
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                if (s_cmd_stop) break;
                /* 恢复播放: 先快速预填充 ring，再切回真实 PCM; 恢复亮度 */
                restore_screen_brightness();
                if (bsp_bt_is_connected()) {
                    /* 预填到留 64KB 余量为止 (旧版填到差 8KB, 真实音频
                     * 恢复推送时余量太小容易被削顶) */
                    while (bsp_bt_pcm_ring_fill() < bsp_bt_pcm_ring_capacity() - 65536) {
                        bsp_bt_pcm_ring_push((const uint8_t *)silence_pcm, sizeof(silence_pcm));
                        vTaskDelay(pdMS_TO_TICKS(1));
                    }
                }
                /* 在恢复点重置 BT 节流时基: 暂停期间目标时刻不推进,
                 * 不重置的话恢复后会爆发式追赶导致大段跳播 */
                s_clock.next_frame_us = esp_timer_get_time();
                set_state(AUDIO_STATE_PLAYING);
            }

            /* 检查 seek */
            if (s_cmd_seek) {
                s_cmd_seek = false;
                audio_decoder_seek(s_seek_target);
                last_progress_sec = 0xFFFFFFFF;
                s_clock.t0_us         = esp_timer_get_time();
                s_clock.next_frame_us = s_clock.t0_us;
                s_clock.total_pcm_frames = 0;
                if (need_src) {
                    src_init(READ_BUF_SIZE / 4);
                }
                ESP_LOGI(TAG, "跳转到: %lus", s_seek_target);
            }

            bool bt_active = bsp_bt_is_connected();

            if (bt_active && !s_fade_in_started) {
                s_fade_in_started = true;
                bsp_bt_start_fade_in();
            }

            /* 读取解码后的 PCM 数据 (按"帧") */
            int frames = audio_decoder_read(pcm_buf, READ_BUF_SIZE / 4);
            if (frames <= 0) {
                ESP_LOGI(TAG, "文件播放完毕: %s", s_music_list[index]);
                break;
            }

            /* 喂入频谱分析模块 (取原始解码帧, SRC 前后均可) */
            fft_spectrum_feed(pcm_buf, frames);

            /* 计算字节数 */
            size_t bytes = (size_t)frames * output_bytes_per_frame;

            if (bt_active) {
                if (need_src) {
                    src_feed(pcm_buf, frames);

                    if (s_src_out_buf != NULL) {
                        int out_frames = src_read(s_src_out_buf, 4096);

                        if (out_frames > 0) {
                            size_t src_bytes = (size_t)out_frames * 4;
                            size_t pushed_now = bsp_bt_pcm_ring_push((const uint8_t *)s_src_out_buf, src_bytes);
                            if (pushed_now == 0) {
                                bsp_bt_pcm_ring_push_blocking((const uint8_t *)s_src_out_buf, src_bytes, 20);
                            }
                            frames = out_frames;
                        }
                    }
                    if (frames <= 0) {
                        continue;
                    }
                } else {
                    /* 推送当前解码的 pcm_buf.
                     * (旧版这里有一套 "preload 预解码缓冲" 逻辑: 每块先推
                     * preload 残留、再推当前块、又把当前块存进 preload —
                     * 由于 refill 条件 2304<4096 永远成立, 循环永不收敛,
                     * 每块 PCM 被重复推送两次, ring 以 2 倍实时速率被灌入
                     * 而持续溢出削顶 = 卡顿+杂音的主因, 已整体移除.
                     * 256KB ring 本身就是 1.45s 的抖动缓冲, 无需预解码池) */
                    size_t pushed_now = bsp_bt_pcm_ring_push((const uint8_t *)pcm_buf, bytes);
                    if (pushed_now == 0) {
                        bsp_bt_pcm_ring_push_blocking((const uint8_t *)pcm_buf, bytes, 20);
                    }
                }

                /* BT 播放节流: "绝对微秒时基" — 目标时刻按累计帧数精确
                 * 累加, 延迟向上取整到 1ms tick。绝对时基不累积误差:
                 * 某块睡过头, 下块的目标仍在原计划上, 长期速率与 SBC
                 * 消费速率(真实采样率)严格一致。
                 * 旧版 vTaskDelayUntil 每块按毫秒截断 (52.24ms→52ms),
                 * 生产比消费快 ~0.5%, ring 满后每块被 push_blocking 的
                 * 20ms 窗口削顶, 丢 10-30ms 音频 = 持续卡顿的根因。 */
                int64_t frame_dur_us = ((int64_t)frames * 1000000) / (int64_t)bt_sample_rate;
                int64_t target_us = s_clock.next_frame_us + frame_dur_us;
                s_clock.next_frame_us = target_us;
                int64_t now_us = esp_timer_get_time();
                if (target_us > now_us) {
                    int64_t wait_us = target_us - now_us;
                    TickType_t delay_ticks =
                        (TickType_t)((wait_us * configTICK_RATE_HZ + 999999) / 1000000);
                    if (delay_ticks < 1) delay_ticks = 1;
                    vTaskDelay(delay_ticks);
                }

                TickType_t now_tick = xTaskGetTickCount();
                if ((now_tick - last_diag_tick) >= configTICK_RATE_HZ) {
                    last_diag_tick = now_tick;
                    size_t fill = bsp_bt_pcm_ring_fill();
                    size_t cap = bsp_bt_pcm_ring_capacity();
                    ESP_LOGD(TAG, "BT ring: %u/%u bytes (%.1f%%)",
                             (unsigned)fill, (unsigned)cap,
                             (double)fill * 100.0 / cap);
                }
            } else {
                /* I2S DAC 路径 */
                size_t bytes_written = 0;
                esp_err_t ret = bsp_audio_write((const uint8_t *)pcm_buf, bytes, &bytes_written);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "音频写入失败: %s", esp_err_to_name(ret));
                    break;
                }
                s_clock.next_frame_us = esp_timer_get_time();
            }

            s_clock.total_pcm_frames += (uint32_t)frames;

            /* 通知进度 */
            audio_info_t cur_info;
            if (audio_decoder_get_info(&cur_info)) {
                uint32_t cur_sec = cur_info.current_seconds;
                if (cur_sec != last_progress_sec) {
                    last_progress_sec = cur_sec;
                    s_last_saved_sec = cur_sec;

                    if (s_progress_cb) {
                        s_progress_cb(index, cur_sec, cur_info.total_seconds,
                                      s_title_list[index]);
                    }

                    if (cur_sec % PROGRESS_SAVE_INTERVAL == 0 && cur_sec > 0) {
                        audio_player_save_progress();
                    }
                }
            }
        }

        /* ================== 单曲结束清理 ================== */
        /* 保存本曲进度: 手动切歌时 s_current_index 已指向新曲, 必须用任务
         * 本地的 index; 自然播完时 s_current_index 可能已被扫描重映射
         * (曲子还在列表里但换了位置), 跟着 s_current_index 才不写错位. */
        save_progress_at(s_cmd_stop ? index : s_current_index, s_last_saved_sec);
        audio_decoder_close();
        bsp_bt_pcm_ring_reset();
        if (need_src) {
            src_deinit();
        }
        set_state(AUDIO_STATE_STOPPED);

        /* s_cmd_stop 语义区分:
         *   s_task_should_exit = true  -> 停止播放/任务重建, 任务退出
         *   s_task_should_exit = false -> 手动切歌, 外层循环直接采用新索引 (无任务重建) */
        if (s_cmd_stop) {
            if (s_task_should_exit) {
                break;
            }
            continue;
        }

        /* 自动切歌: 以 (可能已被扫描重映射的) s_current_index 为基准 */
        int next = next_track_index(s_play_mode, s_current_index);
        if (next < 0) {
            ESP_LOGI(TAG, "顺序播放完毕");
            break;
        }
        ESP_LOGI(TAG, "自动播放下一曲: %d", next);
        s_current_index = next;
    }

    /* ================== 任务退出 ================== */
    /* pcm_buf 常驻不释放 (s_pcm_task_buf), 供下次播放复用 */
    s_cmd_stop = false;
    s_cmd_pause = false;
    s_task_should_exit = false;
    s_player_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief 启动播放任务
 */
static void start_play_task(void)
{
    /* 停止已有任务 (要求任务退出而不是切歌继续) */
    if (s_player_task_handle != NULL) {
        s_cmd_stop = true;
        s_task_should_exit = true;
        int timeout = 100;
        while (s_player_task_handle != NULL && timeout-- > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    s_cmd_stop = false;
    s_cmd_pause = false;
    s_cmd_seek = false;
    s_task_should_exit = false;

    uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t ram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t ram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "PSRAM: %lu/%lu KB free, largest=%lu KB; RAM: %lu KB free, largest=%lu KB",
             (unsigned long)(psram_free / 1024), (unsigned long)(psram_total / 1024),
             (unsigned long)(psram_largest / 1024),
             (unsigned long)(ram_free / 1024), (unsigned long)(ram_largest / 1024));

    BaseType_t ok;
#if (esp_idf_version >= ESP_IDF_VERSION_VAL(5, 2, 0))
    /* 双核分工: 播放任务钉核 1 (与 LVGL 同核, 二者都是"睡眠为主"的周期
     * 任务, 错峰共存); 核 0 留给 Bluedroid SBC 编码 + ESP-Hosted SDIO.
     * 播放任务若随机落到核 0 会被高频 SBC 中断/编码抢占, 造成推流抖动. */
    ok = xTaskCreatePinnedToCoreWithCaps(audio_player_task, "audio_player",
                             PLAYER_TASK_STACK, NULL, PLAYER_TASK_PRIO,
                             &s_player_task_handle, 1,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "PSRAM 任务栈(%d KB)分配失败, 尝试内部RAM", PLAYER_TASK_STACK_BYTES / 1024);
        ok = xTaskCreatePinnedToCore(audio_player_task, "audio_player",
                         PLAYER_TASK_STACK, NULL, PLAYER_TASK_PRIO,
                         &s_player_task_handle, 1);
    }
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "内部RAM任务栈(%d KB)也失败, 尝试缩小到8KB", PLAYER_TASK_STACK_BYTES / 1024);
        ok = xTaskCreatePinnedToCore(audio_player_task, "audio_player",
                         (8 * 1024) / sizeof(StackType_t), NULL, PLAYER_TASK_PRIO,
                         &s_player_task_handle, 1);
    }
#else
    ok = xTaskCreatePinnedToCore(audio_player_task, "audio_player",
                     PLAYER_TASK_STACK, NULL, PLAYER_TASK_PRIO,
                     &s_player_task_handle, 1);
#endif
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "播放任务创建失败 (PSRAM剩余:%lu KB)",
                 (unsigned long)(psram_free / 1024));
        set_state(AUDIO_STATE_STOPPED);
    }
}

/* ================ 公共接口 ================ */

esp_err_t audio_player_init(void)
{
    s_state = AUDIO_STATE_IDLE;
    s_current_index = -1;
    s_music_count = 0;

    /* 在 PSRAM 分配音乐列表 (64 × 128 = 8KB), 统一走 bsp_memory */
    if (s_music_list == NULL) {
        s_music_list = (char (*)[MAX_MUSIC_NAME_LEN])bsp_memory_malloc(
            sizeof(char[MAX_MUSIC_COUNT][MAX_MUSIC_NAME_LEN]));
        if (s_music_list != NULL) {
            memset(s_music_list, 0, sizeof(char[MAX_MUSIC_COUNT][MAX_MUSIC_NAME_LEN]));
        }
    }

    /* ID3 元数据 (PSRAM) */
    if (s_title_list == NULL) {
        s_title_list = (char (*)[128])bsp_memory_malloc(sizeof(char[MAX_MUSIC_COUNT][128]));
        if (s_title_list) memset(s_title_list, 0, sizeof(char[MAX_MUSIC_COUNT][128]));
    }
    if (s_artist_list == NULL) {
        s_artist_list = (char (*)[64])bsp_memory_malloc(sizeof(char[MAX_MUSIC_COUNT][64]));
        if (s_artist_list) memset(s_artist_list, 0, sizeof(char[MAX_MUSIC_COUNT][64]));
    }

    /* LRC 歌词缓冲 (PSRAM): 300 行 × 100B = 30KB */
    if (s_lyrics == NULL) {
        s_lyrics = (lrc_line_t *)bsp_memory_malloc(sizeof(lrc_line_t[LRC_MAX_LINES]));
        if (s_lyrics) memset(s_lyrics, 0, sizeof(lrc_line_t[LRC_MAX_LINES]));
    }
    s_lyrics_count = 0;

    /* SRC 输出缓冲 (PSRAM, 8KB) */
    if (s_src_out_buf == NULL) {
        s_src_out_buf = (int16_t *)bsp_memory_malloc(4096 * sizeof(int16_t));
        if (s_src_out_buf) memset(s_src_out_buf, 0, 4096 * sizeof(int16_t));
    }

    memset(s_fav, 0, sizeof(s_fav));

    if (!s_music_list || !s_title_list || !s_artist_list) {
        ESP_LOGE(TAG, "音频播放器关键列表分配失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "音频播放器初始化完成 (支持WAV/MP3 + ID3/LRC/模式/收藏)");
    return ESP_OK;
}

esp_err_t audio_player_scan_music(const char *dir_path)
{
    if (dir_path == NULL) {
        dir_path = MUSIC_DIR_PATH;
    }

    s_scan_busy = true;

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "无法打开目录: %s", dir_path);
        s_scan_busy = false;
        return ESP_FAIL;
    }

    struct dirent *ent;
    int count = 0;

    /* 正在播放/暂停时整表替换列表会让播放任务手里的索引漂移到另一首歌。
     * 先记住当前曲目文件名, 扫描完成后按名字找回新索引 (scan 不再打断播放). */
    char cur_track_name[MAX_MUSIC_NAME_LEN] = {0};
    bool need_remap = false;
    int old_index = s_current_index;
    if (old_index >= 0 && old_index < s_music_count && s_music_list) {
        copy_fixed(cur_track_name, sizeof(cur_track_name), s_music_list[old_index]);
        need_remap = true;
    }

    while ((ent = readdir(dir)) != NULL && count < MAX_MUSIC_COUNT) {
        bool is_regular = (ent->d_type == DT_REG);
        if (!is_regular && ent->d_type == DT_UNKNOWN) {
            char probe_path[MAX_MUSIC_NAME_LEN + 320];
            struct stat probe_st;
            snprintf(probe_path, sizeof(probe_path), "%s/%s", dir_path, ent->d_name);
            is_regular = (stat(probe_path, &probe_st) == 0 && S_ISREG(probe_st.st_mode));
        }
        if (is_regular) {
            audio_fmt_t fmt = audio_decoder_detect_format(ent->d_name);
            if (fmt == AUDIO_FMT_WAV || fmt == AUDIO_FMT_MP3) {
                size_t name_len = strlen(ent->d_name);
                if (name_len >= MAX_MUSIC_NAME_LEN) {
                    name_len = MAX_MUSIC_NAME_LEN - 1;
                }
                memcpy(s_music_list[count], ent->d_name, name_len);
                s_music_list[count][name_len] = '\0';

                /* 解析 ID3 元数据 */
                char title[128] = {0};
                char artist[64] = {0};
                if (fmt == AUDIO_FMT_MP3) {
                    char fullpath[320];
                    snprintf(fullpath, sizeof(fullpath), "%s/%s", dir_path, ent->d_name);
                    id3_meta_t meta;
                    memset(&meta, 0, sizeof(meta));
                    if (id3_parse(fullpath, &meta)) {
                        if (meta.has_title) copy_fixed(title, sizeof(title), meta.title);
                        if (meta.has_artist) copy_fixed(artist, sizeof(artist), meta.artist);
                    }
                }

                /* 标题回退为文件名 (去掉扩展名) */
                if (title[0] == '\0') {
                    copy_fixed(title, sizeof(title), ent->d_name);
                    char *dot = strrchr(title, '.');
                    if (dot) *dot = '\0';
                }
                if (s_title_list) {
                    copy_fixed(s_title_list[count], 128, title);
                }
                if (s_artist_list) {
                    copy_fixed(s_artist_list[count], 64, artist);
                }

                ESP_LOGD(TAG, "发现音乐: %s (标题: %s / %s)",
                         ent->d_name, title, artist[0] ? artist : "-");
                count++;
            }
        }
    }

    closedir(dir);
    s_music_count = count;

    /* 按文件名重映射当前播放索引 (列表被替换后保持指向同一首歌) */
    if (need_remap && cur_track_name[0]) {
        int new_index = -1;
        for (int i = 0; i < s_music_count; i++) {
            if (strcmp(s_music_list[i], cur_track_name) == 0) {
                new_index = i;
                break;
            }
        }
        if (new_index >= 0) {
            if (new_index != old_index) {
                ESP_LOGI(TAG, "扫描后当前曲目索引重映射: %d -> %d (%s)",
                         old_index, new_index, cur_track_name);
            }
            s_current_index = new_index;
        } else {
            /* 当前曲目已不在新列表中: 索引失效, 停止播放会话避免指向错歌 */
            ESP_LOGW(TAG, "扫描后当前曲目 %s 不在列表中, 停止播放会话",
                     cur_track_name);
            s_current_index = -1;
            if (s_state == AUDIO_STATE_PLAYING || s_state == AUDIO_STATE_PAUSED ||
                s_state == AUDIO_STATE_BUFFERING) {
                s_cmd_stop = true;
                s_cmd_pause = false;
                s_task_should_exit = true;
                set_state(AUDIO_STATE_STOPPED);
            }
        }
    }

    /* 重新加载收藏 */
    audio_player_favorite_init();

    s_scan_busy = false;
    ESP_LOGI(TAG, "扫描完成, 共找到 %d 首音乐", count);
    return ESP_OK;
}

/* ---- 后台扫描: 开机不阻塞 UI, 完成后经回调推送播放列表 ---- */

static void scan_task(void *arg)
{
    (void)arg;
    audio_player_scan_music(MUSIC_DIR_PATH);
    if (s_scan_done_cb) {
        s_scan_done_cb(s_music_count);
    }
    vTaskDelete(NULL);
}

esp_err_t audio_player_scan_music_async(const char *dir_path)
{
    (void)dir_path;   /* 后台扫描固定扫 MUSIC_DIR_PATH */
    if (s_scan_busy) {
        ESP_LOGW(TAG, "扫描已在进行中");
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_busy = true;   /* 先置位再创建任务, 防止任务尚未运行时被重复触发 */

    BaseType_t ok = pdFAIL;
#if (esp_idf_version >= ESP_IDF_VERSION_VAL(5, 2, 0))
    ok = xTaskCreateWithCaps(scan_task, "music_scan",
                             PLAYER_TASK_STACK, NULL, 3, NULL,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "PSRAM 扫描任务栈分配失败, 回退内部 RAM");
    }
#endif
    if (ok != pdPASS) {
        ok = xTaskCreate(scan_task, "music_scan",
                         PLAYER_TASK_STACK, NULL, 3, NULL);
    }
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "扫描任务创建失败");
        s_scan_busy = false;   /* 回滚预置位, 允许下次重试 */
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool audio_player_is_scanning(void)
{
    return s_scan_busy;
}

void audio_player_set_scan_done_cb(void (*cb)(int count))
{
    s_scan_done_cb = cb;
}

int audio_player_get_music_count(void)
{
    return s_music_count;
}

const char* audio_player_get_music_name(int index)
{
    if (index < 0 || index >= s_music_count) {
        return NULL;
    }
    return s_music_list[index];
}

const char *audio_player_get_title(int index)
{
    if (index < 0 || index >= s_music_count) return NULL;
    return s_title_list ? s_title_list[index] : s_music_list[index];
}

const char *audio_player_get_artist(int index)
{
    if (index < 0 || index >= s_music_count) return NULL;
    return s_artist_list ? s_artist_list[index] : "";
}

int audio_player_fill_names(char (*out_names)[128], int max)
{
    if (!out_names || max <= 0) return 0;
    int n = (s_music_count < max) ? s_music_count : max;
    for (int i = 0; i < n; i++) {
        const char *title = audio_player_get_title(i);
        size_t len = strnlen(title, 127);
        memcpy(out_names[i], title, len);
        out_names[i][len] = '\0';
    }
    return n;
}

/* ---- LRC 歌词 ---- */

int audio_player_load_lyrics(int index)
{
    s_lyrics_count = 0;
    if (!s_lyrics || index < 0 || index >= s_music_count) return 0;

    char fullpath[320];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", MUSIC_DIR_PATH, s_music_list[index]);
    s_lyrics_count = lrc_load_for_song(fullpath, s_lyrics, LRC_MAX_LINES);
    if (s_lyrics_count > 0) {
        ESP_LOGI(TAG, "歌词已加载: %d 行", s_lyrics_count);
    }
    return s_lyrics_count;
}

int audio_player_get_lyric_count(void)
{
    return s_lyrics_count;
}

int audio_player_get_lyric_line(uint32_t ms)
{
    return lrc_find_line(s_lyrics, s_lyrics_count, ms);
}

const char *audio_player_get_lyric_text(int line)
{
    if (line < 0 || line >= s_lyrics_count) return "";
    return s_lyrics[line].text;
}

/* ---- 收藏夹 ---- */

void audio_player_favorite_init(void)
{
    memset(s_fav, 0, sizeof(s_fav));
    FILE *f = fopen(FAVORITES_FILE, "r");
    if (!f) return;

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (l == 0) continue;
        for (int i = 0; i < s_music_count; i++) {
            if (strcmp(s_music_list[i], line) == 0) {
                s_fav[i] = true;
                break;
            }
        }
    }
    fclose(f);
}

static void favorite_save(void)
{
    FILE *f = fopen(FAVORITES_FILE, "w");
    if (!f) return;
    for (int i = 0; i < s_music_count; i++) {
        if (s_fav[i]) {
            fprintf(f, "%s\n", s_music_list[i]);
        }
    }
    fclose(f);
}

bool audio_player_is_favorite(int index)
{
    if (index < 0 || index >= s_music_count) return false;
    return s_fav[index];
}

void audio_player_toggle_favorite(int index)
{
    if (index < 0 || index >= s_music_count) return;
    s_fav[index] = !s_fav[index];
    favorite_save();
    ESP_LOGI(TAG, "收藏切换: %d -> %s", index, s_fav[index] ? "是" : "否");
}

/* ---- 睡眠定时 ---- */

void audio_player_set_sleep(uint32_t minutes)
{
    if (minutes == 0) {
        s_sleep_until_ms = 0;
        ESP_LOGI(TAG, "睡眠定时取消");
        return;
    }
    s_sleep_until_ms = (uint64_t)esp_timer_get_time() / 1000 + (uint64_t)minutes * 60000;
    ESP_LOGI(TAG, "睡眠定时: %lu 分钟", (unsigned long)minutes);
}

uint32_t audio_player_get_sleep_remaining(void)
{
    if (s_sleep_until_ms == 0) return 0;
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    if (now_ms >= s_sleep_until_ms) return 0;
    return (uint32_t)((s_sleep_until_ms - now_ms) / 1000);
}

/* ---- 播放模式 ---- */

void audio_player_set_mode(play_mode_t mode)
{
    if (mode < PLAY_MODE_SEQUENCE || mode > PLAY_MODE_REPEAT_ALL) return;
    s_play_mode = mode;
    audio_player_save_progress();   /* 顺带持久化模式 */
    ESP_LOGI(TAG, "播放模式: %d", (int)mode);
}

play_mode_t audio_player_get_mode(void)
{
    return s_play_mode;
}

/* ---- 播放控制 ---- */

/* 请求从 s_current_index 指向的曲目开始播放 (非阻塞, 供控制接口调用):
 * 任务存活 -> 任务内切歌 (置 s_cmd_stop, 任务清理后自动采用新索引);
 * 否则     -> 启动播放任务。调用后立即返回, 播放就绪经 BUFFERING->PLAYING 通知。 */
static void request_track_start(void)
{
    bool task_alive = (s_player_task_handle != NULL) &&
                      (s_state == AUDIO_STATE_PLAYING ||
                       s_state == AUDIO_STATE_PAUSED ||
                       s_state == AUDIO_STATE_BUFFERING);

    set_state(AUDIO_STATE_BUFFERING);
    if (task_alive) {
        s_cmd_stop = true;
        s_cmd_pause = false;
    } else {
        start_play_task();
    }
}

void audio_player_play(void)
{
    if (s_music_count == 0) {
        ESP_LOGW(TAG, "没有可播放的音乐");
        return;
    }

    /* 暂停中: 直接恢复, 不销毁重建任务 (避免进度回退和重建开销) */
    if (s_state == AUDIO_STATE_PAUSED) {
        s_cmd_pause = false;
        ESP_LOGI(TAG, "恢复播放");
        return;
    }

    /* 已在播放/准备中: 忽略重复请求 */
    if (s_state == AUDIO_STATE_PLAYING || s_state == AUDIO_STATE_BUFFERING) {
        return;
    }

    if (s_current_index < 0) {
        int saved_idx;
        uint32_t saved_pos;
        if (audio_player_load_progress(&saved_idx, &saved_pos)) {
            s_current_index = saved_idx;
            ESP_LOGI(TAG, "从上次进度恢复: 索引=%d", saved_idx);
        } else {
            s_current_index = 0;
        }
    }

    request_track_start();
}

void audio_player_pause(void)
{
    if (s_state == AUDIO_STATE_PLAYING) {
        s_cmd_pause = true;
        ESP_LOGI(TAG, "暂停播放");
    }
}

void audio_player_resume(void)
{
    if (s_state == AUDIO_STATE_PAUSED) {
        s_cmd_pause = false;
        ESP_LOGI(TAG, "恢复播放");
    }
}

void audio_player_stop(void)
{
    if (s_state == AUDIO_STATE_PLAYING || s_state == AUDIO_STATE_PAUSED ||
        s_state == AUDIO_STATE_BUFFERING) {
        s_cmd_stop = true;
        s_cmd_pause = false;
        s_task_should_exit = true;   /* 任务完成本曲清理后直接退出 */
        set_state(AUDIO_STATE_STOPPED);
        ESP_LOGI(TAG, "停止播放");
    }
}

void audio_player_next(void)
{
    if (s_music_count == 0) return;

    if (s_play_mode == PLAY_MODE_SHUFFLE) {
        /* 随机模式下下一曲 = 随机 */
        int n = s_music_count;
        s_current_index = (int)(esp_random() % (uint32_t)n);
    } else {
        s_current_index = (s_current_index + 1) % s_music_count;
    }

    if (s_player_task_handle != NULL &&
        (s_state == AUDIO_STATE_PLAYING || s_state == AUDIO_STATE_PAUSED ||
         s_state == AUDIO_STATE_BUFFERING)) {
        /* 任务内切歌: 立即返回, 不在调用线程 (LVGL) 中等待旧任务退出 */
        set_state(AUDIO_STATE_BUFFERING);
        s_cmd_stop = true;
        s_cmd_pause = false;
    }

    ESP_LOGI(TAG, "下一曲: %d - %s", s_current_index,
             s_music_list[s_current_index]);
}

void audio_player_prev(void)
{
    if (s_music_count == 0) return;

    s_current_index--;
    if (s_current_index < 0) {
        s_current_index = s_music_count - 1;
    }

    if (s_player_task_handle != NULL &&
        (s_state == AUDIO_STATE_PLAYING || s_state == AUDIO_STATE_PAUSED ||
         s_state == AUDIO_STATE_BUFFERING)) {
        set_state(AUDIO_STATE_BUFFERING);
        s_cmd_stop = true;
        s_cmd_pause = false;
    }

    ESP_LOGI(TAG, "上一曲: %d - %s", s_current_index,
             s_music_list[s_current_index]);
}

void audio_player_play_index(int index)
{
    if (index < 0 || index >= s_music_count) {
        ESP_LOGE(TAG, "无效的播放索引: %d", index);
        return;
    }

    s_current_index = index;
    request_track_start();
}

void audio_player_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    /* 两条输出路径都要驱动, 否则未连蓝牙时音量条无效:
     *   - BT: 软件增益 (bsp_bt_set_volume)
     *   - I2S: CS43131 硬件音量 (bsp_audio_set_volume 内部已做
     *     I2C 节流 + NVS 防抖保存, 从 LVGL 线程调用安全) */
    bsp_bt_set_volume(volume);
    bsp_audio_set_volume(volume);
}

audio_state_t audio_player_get_state(void)
{
    return s_state;
}

int audio_player_get_current_index(void)
{
    return s_current_index;
}

void audio_player_set_progress_cb(void (*cb)(int index, uint32_t cur,
                                             uint32_t total, const char *title))
{
    s_progress_cb = cb;
}

void audio_player_set_track_cb(void (*cb)(int index))
{
    s_track_cb = cb;
}

void audio_player_set_state_cb(void (*cb)(audio_state_t state))
{
    s_state_cb = cb;
}
