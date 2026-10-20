/**
 * @file audio_decoder.c
 * @brief 统一音频解码器实现 - 支持 WAV 和 MP3 格式
 *
 * 功能:
 *   - 根据文件扩展名自动识别 WAV/MP3 格式
 *   - WAV: 直接读取 PCM 数据, 支持精确 seek
 *   - MP3: 使用 micro-mp3 (OpenCore 固定点 MP3 解码器, 支持 MPEG 1/2/2.5
 *          Layer III, PSRAM 友好, ID3v1/v2 标签自动跳过, 帧同步 + VBR)
 *   - 提供统一的 open/read/seek/close 接口
 *   - 同一时间只支持一个解码器实例(静态变量存储状态)
 *
 * 内存布局:
 *   - MP3 解码缓冲区 (in/out): 内部 SRAM (MALLOC_CAP_8BIT)
 *     micro-mp3 解码器状态和频繁访问的数据用高速内部 SRAM
 *   - WAV 临时 buffer: PSRAM (audio_malloc)
 *   - 所有其他数据 (ring buffer, preload buffer): PSRAM
 */

#include "audio_decoder.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

/* micro-mp3 (OpenCore-based fixed-point MP3 decoder). */
#include "micro_mp3/mp3_c_api.h"

static const char *TAG = "audio_decoder";

/* ================ 常量定义 ================ */

#define MP3_IN_BUF_SIZE     16384          /* MP3 输入缓冲 16KB (PSRAM 优先) */
#define MP3_OUT_BUF_FRAMES  (1152 + 1152)  /* micro-mp3 单次最多解码 1 MPEG1 帧 */
#define MP3_OUT_BUF_SAMPLES (MP3_OUT_BUF_FRAMES * 2)  /* int16 样本数 (stereo worst case) */

/* ================ WAV 文件头 (44字节标准头) ================ */

typedef struct __attribute__((packed)) {
    char     riff[4];          /* "RIFF" */
    uint32_t file_size;        /* 文件大小-8 */
    char     wave[4];          /* "WAVE" */
    char     fmt[4];           /* "fmt " */
    uint32_t fmt_size;         /* fmt 块大小 */
    uint16_t audio_format;     /* 1=PCM */
    uint16_t num_channels;     /* 声道数 */
    uint32_t sample_rate;      /* 采样率 */
    uint32_t byte_rate;        /* 字节率 */
    uint16_t block_align;      /* 块对齐 */
    uint16_t bits_per_sample;  /* 位深 */
    char     data_id[4];       /* "data" */
    uint32_t data_size;        /* 数据大小 */
} wav_header_t;

/* ================ 静态变量 (解码器状态) ================ */

/* 通用状态 */
static audio_fmt_t s_format = AUDIO_FMT_UNKNOWN;
static FILE *s_fp = NULL;
static audio_info_t s_info;

/* WAV 专用状态 */
static uint32_t s_wav_data_offset;     /* PCM 数据起始字节位置 */
static uint32_t s_wav_data_size;       /* PCM 数据总大小(字节) */
static uint32_t s_wav_bytes_played;    /* 已播放字节数 */
static uint32_t s_wav_byte_rate;       /* 字节率 (用于时长计算) */
static int16_t *s_wav_psram_buf = NULL;  /* WAV 临时解码 buffer (PSRAM, ~64KB) */

/* MP3 专用状态 (micro-mp3 / OpenCore) */
static micro_mp3_decoder_t *s_mp3_dec = NULL;
static uint8_t  *s_mp3_in_buf  = NULL;
static int16_t  *s_mp3_out_buf = NULL;
static int      s_mp3_in_offset;        /* 当前 in_buf 消费位置 (字节) */
static int      s_mp3_in_size;          /* 当前 in_buf 有效字节数 */
static long     s_mp3_file_size;        /* 文件总大小 */
static long     s_mp3_bytes_read;       /* 已从文件读取的字节数 (含 ID3v2) */
static uint32_t s_mp3_bitrate_bps;      /* MP3 比特率 bps (估算) */
static bool     s_mp3_info_ready;       /* 已获取 sr/ch 信息 */
static bool     s_mp3_stream_finished;  /* 文件读完了 */
static long     s_mp3_id3v2_skip;       /* 跳过的 ID3v2 tag 大小 */
static int      s_mp3_out_frames;       /* s_mp3_out_buf 中已填好的 stereo 帧数 */
static int      s_mp3_out_pos;          /* 当前 out_buf 消费位置 (帧) */

/* ================ 辅助函数 ================ */

/**
 * @brief 分配缓冲区 (优先 PSRAM，适合 UI 数据、ring buffer 等大块数据)
 */
static void *audio_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ESP_LOGW(TAG, "PSRAM 不足, 改用内部 RAM 分配 %d 字节", (int)size);
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

/**
 * @brief 分配缓冲区 (强制内部 SRAM，适合音频解码等需要高速存取的缓冲区)
 */
static void *audio_malloc_sram(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ESP_LOGW(TAG, "内部 RAM 分配失败: %d 字节", (int)size);
    }
    return ptr;
}

/* ================ 格式检测 ================ */

audio_fmt_t audio_decoder_detect_format(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) {
        return AUDIO_FMT_UNKNOWN;
    }
    if (strcasecmp(ext, ".wav") == 0) {
        return AUDIO_FMT_WAV;
    }
    if (strcasecmp(ext, ".mp3") == 0) {
        return AUDIO_FMT_MP3;
    }
    return AUDIO_FMT_UNKNOWN;
}

/* ================ WAV 解码实现 ================ */

static bool parse_wav_header(void)
{
    wav_header_t header;

    if (fread(&header, sizeof(wav_header_t), 1, s_fp) != 1) {
        ESP_LOGE(TAG, "WAV: 读取文件头失败");
        return false;
    }

    /* 检查 RIFF 标识 */
    if (memcmp(header.riff, "RIFF", 4) != 0) {
        ESP_LOGE(TAG, "WAV: 非 RIFF 文件");
        return false;
    }

    /* 检查 WAVE 标识 */
    if (memcmp(header.wave, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "WAV: 非 WAVE 文件");
        return false;
    }

    /* 检查 PCM 格式 */
    if (header.audio_format != 1) {
        ESP_LOGE(TAG, "WAV: 非 PCM 格式 (format=%d)", header.audio_format);
        return false;
    }

    /* 如果 data_id 不是 "data", 需要搜索 data 块 */
    if (memcmp(header.data_id, "data", 4) != 0) {
        if (header.fmt_size > 16) {
            fseek(s_fp, header.fmt_size - 16, SEEK_CUR);
        }

        char chunk_id[4];
        uint32_t chunk_size;
        while (fread(chunk_id, 4, 1, s_fp) == 1) {
            if (fread(&chunk_size, 4, 1, s_fp) != 1) {
                break;
            }
            if (memcmp(chunk_id, "data", 4) == 0) {
                header.data_size = chunk_size;
                s_wav_data_offset = ftell(s_fp);
                goto wav_header_done;
            }
            fseek(s_fp, chunk_size, SEEK_CUR);
        }
        ESP_LOGE(TAG, "WAV: 未找到 data 块");
        return false;
    }

    s_wav_data_offset = ftell(s_fp);

wav_header_done:
    s_wav_data_size = header.data_size;
    s_wav_byte_rate = header.byte_rate;
    s_wav_bytes_played = 0;

    s_info.format = AUDIO_FMT_WAV;
    s_info.sample_rate = header.sample_rate;
    s_info.channels = header.num_channels;
    s_info.bits_per_sample = header.bits_per_sample;
    s_info.total_seconds = (header.byte_rate > 0)
                           ? (header.data_size / header.byte_rate) : 0;
    s_info.current_seconds = 0;
    s_info.total_samples = (header.num_channels > 0 && header.bits_per_sample > 0)
                           ? (header.data_size / (header.num_channels * header.bits_per_sample / 8))
                           : 0;

    ESP_LOGI(TAG, "WAV: %luHz, %dch, %dbit, 时长 %lus",
             (unsigned long)s_info.sample_rate, s_info.channels,
             s_info.bits_per_sample, s_info.total_seconds);
    return true;
}

static int wav_decoder_read(int16_t *pcm_buf, int max_frames)
{
    /* 始终输出 stereo 16bit PCM; mono 源复制 L=R.
     * 1 frame = 4 字节 (stereo int16). */
    const int out_bytes_per_frame = 2 * 2;
    int src_channels = (s_info.channels > 0) ? s_info.channels : 2;
    int src_bytes_per_frame = src_channels * (s_info.bits_per_sample / 8);
    if (src_bytes_per_frame <= 0) src_bytes_per_frame = out_bytes_per_frame;

    /* 确保 PSRAM 临时 buffer 已分配 */
    int max_src_bytes = max_frames * src_bytes_per_frame;
    if (s_wav_psram_buf == NULL) {
        /* 首次分配: 最多 16384 帧 × 4字节 = 64KB */
        int alloc_frames = max_frames > 16384 ? 16384 : max_frames;
        s_wav_psram_buf = (int16_t *)audio_malloc(alloc_frames * src_bytes_per_frame);
        if (s_wav_psram_buf == NULL) return 0;
    }
    int16_t *tmp = s_wav_psram_buf;

    size_t bytes_read = fread(tmp, 1, max_src_bytes, s_fp);
    int src_frames_read = (int)(bytes_read / src_bytes_per_frame);
    if (src_frames_read <= 0) {
        return 0;
    }

    if (src_channels == 1) {
        for (int i = 0; i < src_frames_read; i++) {
            int16_t s = tmp[i];
            pcm_buf[i * 2]     = s;
            pcm_buf[i * 2 + 1] = s;
        }
    } else {
        memcpy(pcm_buf, tmp, (size_t)src_frames_read * src_bytes_per_frame);
    }

    s_wav_bytes_played += bytes_read;
    s_info.current_seconds = (s_wav_byte_rate > 0)
                             ? (s_wav_bytes_played / s_wav_byte_rate) : 0;

    return src_frames_read;
}

static bool wav_decoder_seek(uint32_t seconds)
{
    if (s_wav_byte_rate == 0) return false;

    long target_byte = (long)seconds * s_wav_byte_rate;
    if (target_byte > (long)s_wav_data_size) target_byte = s_wav_data_size;

    int bytes_per_sample = s_info.channels * s_info.bits_per_sample / 8;
    if (bytes_per_sample > 0) {
        target_byte = (target_byte / bytes_per_sample) * bytes_per_sample;
    }

    if (fseek(s_fp, s_wav_data_offset + target_byte, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "WAV: seek 失败");
        return false;
    }

    s_wav_bytes_played = target_byte;
    s_info.current_seconds = seconds;
    return true;
}

/* ================ MP3 解码实现 (micro-mp3 / OpenCore) ================ */

/**
 * @brief 把 in_buf 剩余的字节前移, 然后追加 fread 读到的文件数据.
 *
 * compact-fill: 比单纯 fread 16K 好处在于 - 旧数据先被留下, 新数据补到末尾,
 * 窗口永远保持 [old_leftover, old_leftover + new_bytes), 便于解码器解析.
 */
static bool mp3_refill_input(void)
{
    if (s_mp3_in_offset > 0 && s_mp3_in_offset < s_mp3_in_size) {
        int leftover = s_mp3_in_size - s_mp3_in_offset;
        memmove(s_mp3_in_buf, s_mp3_in_buf + s_mp3_in_offset, (size_t)leftover);
        s_mp3_in_size = leftover;
        s_mp3_in_offset = 0;
    } else if (s_mp3_in_offset >= s_mp3_in_size) {
        s_mp3_in_offset = 0;
        s_mp3_in_size = 0;
    }

    if (s_mp3_stream_finished) {
        return (s_mp3_in_size > 0);
    }

    size_t want = (size_t)(MP3_IN_BUF_SIZE - s_mp3_in_size);
    if (want == 0) return true;

    size_t got = fread(s_mp3_in_buf + s_mp3_in_size, 1, want, s_fp);
    if (got > 0) {
        s_mp3_bytes_read += (long)got;
        s_mp3_in_size += (int)got;
    }
    if (got == 0) {
        s_mp3_stream_finished = true;
    }
    return (s_mp3_in_size > 0);
}

/**
 * @brief 在第一次拿到 stream info 时填 s_info.
 */
static void mp3_try_update_info(void)
{
    uint32_t sr = micro_mp3_decoder_get_sample_rate(s_mp3_dec);
    uint8_t  ch = micro_mp3_decoder_get_channels(s_mp3_dec);
    uint32_t br_kbps = micro_mp3_decoder_get_bitrate(s_mp3_dec);

    if (sr == 0 || ch == 0) return;

    s_info.sample_rate = sr;
    s_info.channels    = ch;
    s_info.bits_per_sample = 16;

    if (br_kbps > 0) {
        s_mp3_bitrate_bps = br_kbps * 1000;
        if (s_mp3_file_size > 0 && s_info.total_seconds == 0) {
            s_info.total_seconds = (uint32_t)((uint64_t)s_mp3_file_size * 8 / s_mp3_bitrate_bps);
        }
    }

    if (!s_mp3_info_ready) {
        ESP_LOGI(TAG, "MP3: %luHz, %dch, ~%ukbps, ~%lus",
                 (unsigned long)sr, ch, br_kbps, s_info.total_seconds);
        s_mp3_info_ready = true;
    }
}

/**
 * @brief 解码一帧到 s_mp3_out_buf; 返回这次产出的 stereo 帧数.
 *
 * micro-mp3 是流式 API, 内部维护帧窗口 + 残余字节; 单次调用解码一帧.
 * 返回的 frames 是"每声道样本数"也就是 1 帧 = channels 个 int16.
 * 我们的输出 PCM 是 stereo: mono 来源会被复制到 L=R.
 */
static int mp3_decode_one_frame(void)
{
    if (s_mp3_in_offset >= s_mp3_in_size && !mp3_refill_input()) {
        return 0;
    }

    size_t in_pos   = (size_t)s_mp3_in_offset;
    size_t in_end   = (size_t)s_mp3_in_size;
    size_t consumed = 0;
    size_t frames_dec = 0;

    int rc = micro_mp3_decoder_decode(
        s_mp3_dec,
        s_mp3_in_buf + in_pos,
        in_end - in_pos,
        s_mp3_out_buf,
        (size_t)MP3_OUT_BUF_FRAMES,
        &consumed,
        &frames_dec);

    s_mp3_in_offset += (int)consumed;

    switch (rc) {
    case 0:   /* MP3_OK */
        break;
    case 1:   /* MP3_NEED_MORE_DATA - 还差一截, 后续重读 */
        mp3_refill_input();
        return 0;
    case 2:   /* MP3_STREAM_INFO_READY - 解码器刚解析到帧头, 记录信息即可 */
        mp3_try_update_info();
        return 0;
    case -4:  /* MP3_DECODE_ERROR - 当前帧损坏, micro-mp3 已经自同步跳过 */
        ESP_LOGD(TAG, "MP3: 当前帧损坏, 已跳过");
        return 0;
    case -3:  /* MP3_OUTPUT_BUFFER_TOO_SMALL */
        ESP_LOGW(TAG, "MP3: 输出缓冲不足, 跳过此帧");
        return 0;
    case -1:  /* MP3_INPUT_INVALID */
        ESP_LOGE(TAG, "MP3: 输入数据无效");
        return 0;
    case -2:  /* MP3_ALLOCATION_FAILED */
        ESP_LOGE(TAG, "MP3: micro-mp3 内存分配失败");
        return 0;
    default:
        if (rc < 0) {
            ESP_LOGW(TAG, "MP3: 解码器错误 %d, 重置", rc);
            micro_mp3_decoder_reset(s_mp3_dec);
            s_mp3_info_ready = false;
            return 0;
        }
        break;
    }

    if (frames_dec == 0) return 0;

    /* 标准 micro-mp3 输出: int16 interleaved, channels = channels_in_file (1 或 2).
     * 我们的调用方要 stereo, 因此 mono 源需要复制 L=R. */
    uint8_t ch = (s_info.channels > 0) ? s_info.channels : 2;
    if (ch == 1) {
        /* 把 [L, ?, L, ?, ...] 整理为 [L=R, L=R, ...],
         * 复制完后保留前 frames_dec * 2 个 int16 = frames_dec 个 stereo 帧. */
        for (int i = (int)frames_dec - 1; i >= 0; i--) {
            int16_t s = s_mp3_out_buf[i * 2 + 0];
            s_mp3_out_buf[i * 2 + 0] = s;
            s_mp3_out_buf[i * 2 + 1] = s;
        }
    }

    /* 实时刷新比特率 (VBR 时每次可能变) */
    mp3_try_update_info();

    return (int)frames_dec;
}

static bool mp3_decoder_open(audio_info_t *info)
{
    /* 音频解码缓冲区放内部 SRAM: micro-mp3 访问频繁，PSRAM 延迟太高影响解码性能 */
    s_mp3_in_buf  = (uint8_t  *)audio_malloc_sram(MP3_IN_BUF_SIZE);
    s_mp3_out_buf = (int16_t  *)audio_malloc_sram(MP3_OUT_BUF_SAMPLES * sizeof(int16_t));
    if (!s_mp3_in_buf || !s_mp3_out_buf) {
        ESP_LOGE(TAG, "MP3: 缓冲区分配失败");
        if (s_mp3_in_buf)  { free(s_mp3_in_buf);  s_mp3_in_buf  = NULL; }
        if (s_mp3_out_buf) { free(s_mp3_out_buf); s_mp3_out_buf = NULL; }
        return false;
    }
    ESP_LOGI(TAG, "MP3: 缓冲区已分配 (in:%d, out:%d 字节), 内部 SRAM",
             MP3_IN_BUF_SIZE, (int)(MP3_OUT_BUF_SAMPLES * sizeof(int16_t)));

    long cur = ftell(s_fp);
    fseek(s_fp, 0, SEEK_END);
    s_mp3_file_size = ftell(s_fp);
    fseek(s_fp, cur, SEEK_SET);

    s_mp3_dec = micro_mp3_decoder_create();
    if (!s_mp3_dec) {
        ESP_LOGE(TAG, "MP3: 创建 micro-mp3 解码器失败");
        free(s_mp3_in_buf);  s_mp3_in_buf  = NULL;
        free(s_mp3_out_buf); s_mp3_out_buf = NULL;
        return false;
    }

    s_mp3_in_offset     = 0;
    s_mp3_in_size       = 0;
    s_mp3_out_pos       = 0;
    s_mp3_out_frames    = 0;
    s_mp3_bytes_read    = 0;
    s_mp3_bitrate_bps   = 0;
    s_mp3_info_ready    = false;
    s_mp3_stream_finished = false;
    s_mp3_id3v2_skip    = 0;

    /* micro-mp3 内部也会跳过 ID3v2, 这里我们手动同步字节计数.
     * "ID3" + version(2) + flags(1) + syncsafe size(4) */
    {
        unsigned char head[10];
        long save = ftell(s_fp);
        fseek(s_fp, 0, SEEK_SET);
        if (fread(head, 1, 10, s_fp) == 10 && memcmp(head, "ID3", 3) == 0) {
            uint32_t sz = ((uint32_t)(head[6] & 0x7F) << 21) |
                          ((uint32_t)(head[7] & 0x7F) << 14) |
                          ((uint32_t)(head[8] & 0x7F) << 7)  |
                          ((uint32_t)(head[9] & 0x7F));
            s_mp3_id3v2_skip = 10 + (long)sz;
            if (s_mp3_id3v2_skip >= s_mp3_file_size) {
                s_mp3_id3v2_skip = 0;
                fseek(s_fp, save, SEEK_SET);
            } else {
                fseek(s_fp, s_mp3_id3v2_skip, SEEK_SET);
                s_mp3_bytes_read = s_mp3_id3v2_skip;
                ESP_LOGI(TAG, "MP3: 跳过开头 ID3v2 tag, %ld 字节", s_mp3_id3v2_skip);
            }
        } else {
            fseek(s_fp, save, SEEK_SET);
        }
    }

    s_info.format = AUDIO_FMT_MP3;
    s_info.bits_per_sample = 16;
    s_info.current_seconds = 0;
    s_info.total_samples  = 0;
    s_info.total_seconds  = 0;

    /* MP3 采样率/声道要从第一个解码帧里获取，这里预解码一帧填充 s_info.
     * 避免 player 用 sample_rate=0 计算 frame_dur_ms 导致 vTaskDelayUntil(0) 卡死。 */
    s_mp3_in_offset  = 0;
    s_mp3_in_size    = 0;
    s_mp3_out_pos    = 0;
    s_mp3_out_frames = 0;
    int first_frames = mp3_decode_one_frame();
    if (first_frames > 0) {
        s_info.sample_rate = micro_mp3_decoder_get_sample_rate(s_mp3_dec);
        s_info.channels    = micro_mp3_decoder_get_channels(s_mp3_dec);
        if (s_mp3_bitrate_bps > 0 && s_info.sample_rate > 0) {
            s_info.total_seconds = (uint32_t)((uint64_t)(s_mp3_file_size - s_mp3_id3v2_skip) * 8
                                              / s_mp3_bitrate_bps);
        }
        ESP_LOGI(TAG, "MP3: %luHz, %uch, ~%lukbps, ~%us",
                 (unsigned long)s_info.sample_rate, s_info.channels,
                 (unsigned long)(s_mp3_bitrate_bps / 1000), s_info.total_seconds);
    }

    if (info) {
        *info = s_info;
    }

    ESP_LOGI(TAG, "MP3: 解码器已打开 (micro-mp3/OpenCore), 文件大小 %ld 字节",
             s_mp3_file_size);
    return true;
}

static int mp3_decoder_read(int16_t *pcm_buf, int max_frames)
{
    /* 输出格式约定: 始终为 stereo int16, 1 frame = 4 字节 (L + R),
     * mono 源已在上一步复制为 L=R. */
    int total_frames = 0;

    while (total_frames < max_frames) {
        /* 1. 先消费已经解码好的 PCM */
        if (s_mp3_out_pos < s_mp3_out_frames) {
            int avail  = s_mp3_out_frames - s_mp3_out_pos;
            int remain = max_frames - total_frames;
            int copy   = (avail < remain) ? avail : remain;
            if (copy <= 0) break;

            memcpy(pcm_buf + total_frames * 2,
                   s_mp3_out_buf + s_mp3_out_pos * 2,
                   (size_t)copy * 2 * sizeof(int16_t));
            s_mp3_out_pos += copy;
            total_frames  += copy;
            continue;
        }

        /* 2. 重新解码一帧 */
        s_mp3_out_pos = 0;
        s_mp3_out_frames = mp3_decode_one_frame();
        if (s_mp3_out_frames <= 0) {
            if (s_mp3_stream_finished && s_mp3_in_offset >= s_mp3_in_size) {
                break;   /* EOF */
            }
            /* 否则继续解码 */
            continue;
        }
    }

    /* 更新当前播放进度 (基于 bitrate 估算) */
    if (s_mp3_bitrate_bps > 0) {
        long consumed_after_id3 = s_mp3_bytes_read - s_mp3_id3v2_skip;
        if (consumed_after_id3 < 0) consumed_after_id3 = 0;
        s_info.current_seconds = (uint32_t)((uint64_t)consumed_after_id3 * 8 / s_mp3_bitrate_bps);
        if (s_info.total_seconds > 0 && s_info.current_seconds > s_info.total_seconds) {
            s_info.current_seconds = s_info.total_seconds;
        }
    } else if (s_info.total_seconds > 0 && s_mp3_file_size > 0) {
        long consumed_after_id3 = s_mp3_bytes_read - s_mp3_id3v2_skip;
        if (consumed_after_id3 < 0) consumed_after_id3 = 0;
        s_info.current_seconds = (uint32_t)((uint64_t)s_info.total_seconds
                                            * consumed_after_id3 / s_mp3_file_size);
    }

    return total_frames;
}

static bool mp3_decoder_seek(uint32_t seconds)
{
    if (s_mp3_file_size <= 0) return false;

    /* MP3 没有精确 seek 表 (CBR/VBR 各自差距很大), 这里用估算字节位置;
     * micro-mp3 会从该位置向前找同步字, 自动重同步. */
    long target_byte = 0;
    if (s_mp3_bitrate_bps > 0) {
        target_byte = (long)((uint64_t)seconds * s_mp3_bitrate_bps / 8);
    } else if (s_info.total_seconds > 0) {
        target_byte = (long)((uint64_t)seconds * s_mp3_file_size / s_info.total_seconds);
    } else {
        ESP_LOGW(TAG, "MP3: seek 失败, 无法估算目标字节位置");
        return false;
    }

    /* 留出 ID3v2 tag 偏移 */
    target_byte += s_mp3_id3v2_skip;
    if (target_byte >= s_mp3_file_size) target_byte = s_mp3_file_size;
    if (target_byte <  s_mp3_id3v2_skip) target_byte = s_mp3_id3v2_skip;

    if (fseek(s_fp, target_byte, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "MP3: seek 失败");
        return false;
    }

    /* 重置内部缓冲 + 解码器 - 解码器会从新位置重新找同步字 */
    s_mp3_in_offset = 0;
    s_mp3_in_size   = 0;
    s_mp3_out_pos    = 0;
    s_mp3_out_frames = 0;
    s_mp3_bytes_read = target_byte;
    s_mp3_stream_finished = false;
    s_mp3_info_ready = false;
    micro_mp3_decoder_reset(s_mp3_dec);

    s_info.current_seconds = seconds;
    ESP_LOGI(TAG, "MP3: seek 到 %us (字节 %ld)", seconds, target_byte);
    return true;
}

static void mp3_decoder_close(void)
{
    if (s_mp3_dec) {
        micro_mp3_decoder_destroy(s_mp3_dec);
        s_mp3_dec = NULL;
    }
    if (s_mp3_in_buf) {
        free(s_mp3_in_buf);
        s_mp3_in_buf = NULL;
    }
    if (s_mp3_out_buf) {
        free(s_mp3_out_buf);
        s_mp3_out_buf = NULL;
    }
    s_mp3_in_offset = 0;
    s_mp3_in_size   = 0;
    s_mp3_out_pos    = 0;
    s_mp3_out_frames = 0;
    s_mp3_file_size  = 0;
    s_mp3_bytes_read = 0;
    s_mp3_bitrate_bps = 0;
    s_mp3_info_ready = false;
    s_mp3_stream_finished = false;
    s_mp3_id3v2_skip = 0;
}

/* ================ 统一接口实现 ================ */

bool audio_decoder_open(const char *filepath, audio_info_t *info)
{
    audio_decoder_close();

    if (!filepath) {
        ESP_LOGE(TAG, "文件路径为空");
        return false;
    }

    s_format = audio_decoder_detect_format(filepath);
    if (s_format == AUDIO_FMT_UNKNOWN) {
        ESP_LOGE(TAG, "不支持的文件格式: %s", filepath);
        return false;
    }

    s_fp = fopen(filepath, "rb");
    if (!s_fp) {
        ESP_LOGE(TAG, "无法打开文件: %s", filepath);
        s_format = AUDIO_FMT_UNKNOWN;
        return false;
    }

    bool result = false;
    switch (s_format) {
    case AUDIO_FMT_WAV:
        result = parse_wav_header();
        break;
    case AUDIO_FMT_MP3:
        result = mp3_decoder_open(NULL);
        break;
    default:
        break;
    }

    if (!result) {
        if (s_fp) {
            fclose(s_fp);
            s_fp = NULL;
        }
        s_format = AUDIO_FMT_UNKNOWN;
        return false;
    }

    if (info) {
        *info = s_info;
    }

    ESP_LOGI(TAG, "已打开: %s (格式: %d)", filepath, s_format);
    return true;
}

int audio_decoder_read(int16_t *pcm_buf, int max_frames)
{
    if (!s_fp || s_format == AUDIO_FMT_UNKNOWN || !pcm_buf || max_frames <= 0) {
        return 0;
    }
    switch (s_format) {
    case AUDIO_FMT_WAV:
        return wav_decoder_read(pcm_buf, max_frames);
    case AUDIO_FMT_MP3:
        return mp3_decoder_read(pcm_buf, max_frames);
    default:
        return 0;
    }
}

bool audio_decoder_seek(uint32_t seconds)
{
    if (!s_fp || s_format == AUDIO_FMT_UNKNOWN) return false;
    switch (s_format) {
    case AUDIO_FMT_WAV:
        return wav_decoder_seek(seconds);
    case AUDIO_FMT_MP3:
        return mp3_decoder_seek(seconds);
    default:
        return false;
    }
}

void audio_decoder_close(void)
{
    if (s_format == AUDIO_FMT_MP3) {
        mp3_decoder_close();
    } else if (s_format == AUDIO_FMT_WAV) {
        /* 释放 WAV PSRAM 临时 buffer */
        if (s_wav_psram_buf != NULL) {
            free(s_wav_psram_buf);
            s_wav_psram_buf = NULL;
        }
    }
    if (s_fp) {
        fclose(s_fp);
        s_fp = NULL;
    }
    s_format = AUDIO_FMT_UNKNOWN;
    memset(&s_info, 0, sizeof(s_info));
}

bool audio_decoder_get_info(audio_info_t *info)
{
    if (!info || s_format == AUDIO_FMT_UNKNOWN) return false;
    *info = s_info;
    return true;
}
