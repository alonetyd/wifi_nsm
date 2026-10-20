/**
 * @file id3_tags.c
 * @brief MP3 ID3 标签解析实现 (ID3v1 / ID3v2.2 / v2.3 / v2.4)
 *
 * 支持:
 *   - ID3v2.4: 4字符帧ID + syncsafe 大小, 无格式标志位 (v2.4 只有 flag bits)
 *   - ID3v2.3: 4字符帧ID + 4字节大端大小
 *   - ID3v2.2: 3字符帧ID + 3字节大小
 *   - ID3v1:   文件末尾 128 字节
 *   - 文本编码: 0=ISO-8859-1(可能是GBK), 1=UTF-16(BOM), 2=UTF-16BE, 3=UTF-8
 *   - APIC 封面提取 (可选)
 */

#include "id3_tags.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "bsp_memory.h"

static const char *TAG = "id3";

/* ================ UTF-8 / GBK 工具 ================ */

/* GBK 双字节 -> Unicode 码点 (定义于 gbk_to_unicode.c) */
extern uint32_t gbk_to_unicode_lookup(uint16_t gbk_code);

/* 写一个 Unicode 码点到 UTF-8 缓冲, 返回字节数 */
static int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* 判断字符串是不是合法 UTF-8 */
static bool looks_like_utf8(const char *s)
{
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        if (c < 0x80) continue;
        int n;
        if ((c & 0xE0) == 0xC0)      n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if ((c & 0xF8) == 0xF0) n = 3;
        else return false;
        for (int i = 0; i < n; i++) {
            if (s[1 + i] == '\0' || ((uint8_t)s[1 + i] & 0xC0) != 0x80) return false;
        }
        s += n;
    }
    return true;
}

/* 把可能是 GBK 的字节流转写为 UTF-8 */
size_t id3_normalize_text(const char *src, char *dst, size_t dst_cap)
{
    if (!src || !dst || dst_cap == 0) return 0;

    if (looks_like_utf8(src)) {
        size_t n = strlen(src);
        if (n >= dst_cap) n = dst_cap - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
        return n;
    }

    size_t out = 0;
    const uint8_t *p = (const uint8_t *)src;
    while (*p && out + 4 < dst_cap) {
        if (*p < 0x80) {
            dst[out++] = (char)*p++;
            continue;
        }
        uint8_t b2 = p[1];
        if (b2 == 0) break;
        /* GBK 低位不合法时原样输出 */
        if (b2 < 0x40 || b2 == 0x7F) {
            dst[out++] = (char)*p++;
            continue;
        }
        uint16_t gbk_code = (uint16_t)((*p << 8) | b2);
        uint32_t cp = gbk_to_unicode_lookup(gbk_code);
        if (cp == 0) {
            dst[out++] = (char)*p++;
            continue;
        }
        out += utf8_encode(cp, dst + out);
        p += 2;
    }
    dst[out] = '\0';
    return out;
}

/* UTF-16 -> UTF-8, 支持 BOM 探测; 返回实际转换字节数 */
static int utf16_to_utf8(const uint8_t *src, size_t src_len, bool is_be, char *dst, size_t dst_cap)
{
    size_t out = 0;
    size_t i = 0;

    /* 跳过 BOM */
    if (src_len >= 2 && ((src[0] == 0xFF && src[1] == 0xFE) || (src[0] == 0xFE && src[1] == 0xFF))) {
        is_be = (src[0] == 0xFE);
        i = 2;
    }

    while (i + 1 < src_len && out + 4 < dst_cap) {
        uint16_t unit;
        if (is_be) {
            unit = (uint16_t)((src[i] << 8) | src[i + 1]);
        } else {
            unit = (uint16_t)(src[i] | (src[i + 1] << 8));
        }
        i += 2;

        uint32_t cp = unit;
        if (unit == 0) break; /* 字符串结束 */
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < src_len) {
            uint16_t lo;
            if (is_be) {
                lo = (uint16_t)((src[i] << 8) | src[i + 1]);
            } else {
                lo = (uint16_t)(src[i] | (src[i + 1] << 8));
            }
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((unit - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        out += utf8_encode(cp, dst + out);
    }
    dst[out] = '\0';
    return (int)out;
}

/* ================ ID3v2 文本帧读取 ================ */

/* 从文本帧数据中读取字符串 (根据 encoding 字节), 输出 UTF-8 */
static void read_text_frame(const uint8_t *data, size_t len, char *out, size_t out_cap)
{
    if (len == 0 || out_cap == 0) { if (out_cap) out[0] = '\0'; return; }
    uint8_t enc = data[0];
    const uint8_t *text = data + 1;
    size_t text_len = len - 1;

    switch (enc) {
    case 1:  /* UTF-16 with BOM */
    case 2:  /* UTF-16BE */
        utf16_to_utf8(text, text_len, (enc == 2), out, out_cap);
        break;
    case 3:  /* UTF-8 */
    default: /* 0 = ISO-8859-1, 可能是 GBK */
    {
        /* 复制到临时缓冲, 去掉结尾 '\0' */
        char tmp[256];
        size_t n = (text_len < sizeof(tmp) - 1) ? text_len : (sizeof(tmp) - 1);
        memcpy(tmp, text, n);
        tmp[n] = '\0';
        /* 去除字符串中间的 \0 (如 ISO-8859-1 以 \0 结尾的情况) */
        size_t real = strlen(tmp);
        id3_normalize_text(tmp, out, out_cap);
        (void)real;
        break;
    }
    }
}

/* syncsafe 32位 (ID3v2.4) */
static uint32_t read_syncsafe(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) |
           ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  |
           ((uint32_t)(p[3] & 0x7F));
}

/* 解析 ID3v2 tag (已跳过 10 字节头). 返回是否解析到任何字段 */
static void parse_id3v2(FILE *f, uint32_t tag_size, id3_meta_t *meta)
{
    /* 整个 tag 读入 PSRAM 优先堆 (通常 < 64KB) */
    uint32_t alloc = (tag_size < 256 * 1024) ? tag_size : (256 * 1024);
    uint8_t *buf = (uint8_t *)bsp_memory_malloc(alloc ? alloc : 1);
    if (!buf) {
        ESP_LOGW(TAG, "ID3v2 分配失败 (%lu 字节)", (unsigned long)alloc);
        return;
    }
    size_t got = fread(buf, 1, alloc, f);
    if (got < 10) {
        free(buf);
        return;
    }

    size_t pos = 0;
    const size_t end = got;
    char frame_id[5];

    while (pos + 6 <= end) {
        uint32_t frame_size = 0;

        if (buf[pos] == 0) {
            /* v2.4 帧间有 0 填充, 跳过 */
            pos++;
            continue;
        }

        /* 帧 ID 判断版本: 以字符开头 */
        const char *id = (const char *)&buf[pos];
        bool valid_id = true;
        for (int i = 0; i < 3; i++) {
            char c = id[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) { valid_id = false; break; }
        }

        if (!valid_id) {
            /* 可能是 v2.4 的扩展头 (tag 头 flags 0x40) 或填充区域, 跳出 */
            break;
        }

        /* v2.3/v2.4 帧 ID 为 4 字符 */
        bool four = (pos + 10 <= end);
        if (four) {
            char c4 = id[3];
            four = (c4 >= 'A' && c4 <= 'Z') || (c4 >= '0' && c4 <= '9');
        }

        if (four) {
            /* 4 字符帧: 大小可能为 syncsafe (v2.4) 或大端 (v2.3).
             * 判定: v2.4 高位 bit 一定是 0. 用值大小粗判即可:
             * 若 buf[pos+4] 最高位为 0 且 buf[pos+4]>=0x20 多为大端; 但两者相差有限.
             * 稳妥做法: 读两种, 取落在合理范围 (0 <= size <= tag_size) 者. */
            uint32_t big = ((uint32_t)buf[pos + 4] << 24) | ((uint32_t)buf[pos + 5] << 16) |
                           ((uint32_t)buf[pos + 6] << 8) | (uint32_t)buf[pos + 7];
            uint32_t ss  = read_syncsafe(&buf[pos + 4]);
            if (big > 0 && big <= tag_size && ss <= tag_size) {
                /* 两者都合理: v2.3 大端高位通常非 0, syncsafe 高位恒 0 */
                frame_size = (buf[pos + 4] & 0x80) ? big : ss;
            } else if (big > 0 && big <= tag_size) {
                frame_size = big;
            } else {
                frame_size = ss;
            }
            frame_id[0] = id[0]; frame_id[1] = id[1]; frame_id[2] = id[2]; frame_id[3] = id[3];
            frame_id[4] = '\0';
            pos += 10;
        } else {
            /* 3 字符帧 (v2.2) */
            frame_size = ((uint32_t)buf[pos + 3] << 16) | ((uint32_t)buf[pos + 4] << 8) | buf[pos + 5];
            frame_id[0] = id[0]; frame_id[1] = id[1]; frame_id[2] = id[2]; frame_id[3] = '\0'; frame_id[4] = '\0';
            pos += 6;
        }

        if (frame_size == 0) continue;
        if (pos + frame_size > end) {
            /* 帧数据超出已读范围, 结束解析 */
            break;
        }

        const uint8_t *fdata = &buf[pos];

        /* 文本帧 */
        bool is_text = (fdata[0] <= 3) && (frame_size >= 1);
        const char *fid = (const char *)frame_id;
        if (is_text) {
            if (strcmp(fid, "TIT2") == 0 || strcmp(fid, "TT2") == 0) {
                if (!meta->has_title) {
                    read_text_frame(fdata, frame_size, meta->title, sizeof(meta->title));
                    meta->has_title = (meta->title[0] != '\0');
                }
            } else if (strcmp(fid, "TPE1") == 0 || strcmp(fid, "TP1") == 0) {
                if (!meta->has_artist) {
                    read_text_frame(fdata, frame_size, meta->artist, sizeof(meta->artist));
                    meta->has_artist = (meta->artist[0] != '\0');
                }
            } else if (strcmp(fid, "TALB") == 0 || strcmp(fid, "TAL") == 0) {
                if (!meta->has_album) {
                    read_text_frame(fdata, frame_size, meta->album, sizeof(meta->album));
                    meta->has_album = (meta->album[0] != '\0');
                }
            }
        }

        pos += frame_size;
        if (meta->has_title && meta->has_artist) break;
    }

    free(buf);
}

/* ================ ID3v1 ================ */

static void parse_id3v1(FILE *f, id3_meta_t *meta)
{
    if (fseek(f, -128, SEEK_END) != 0) return;
    unsigned char tag[128];
    if (fread(tag, 1, 128, f) != 128) return;
    if (memcmp(tag, "TAG", 3) != 0) return;

    char tmp[128];
    memcpy(tmp, tag + 3, 30);
    tmp[30] = '\0';
    if (tmp[0] && !meta->has_title) {
        id3_normalize_text(tmp, meta->title, sizeof(meta->title));
        meta->has_title = (meta->title[0] != '\0');
    }
    memcpy(tmp, tag + 33, 30);
    tmp[30] = '\0';
    if (tmp[0] && !meta->has_artist) {
        id3_normalize_text(tmp, meta->artist, sizeof(meta->artist));
        meta->has_artist = (meta->artist[0] != '\0');
    }
    memcpy(tmp, tag + 63, 30);
    tmp[30] = '\0';
    if (tmp[0] && !meta->has_album) {
        id3_normalize_text(tmp, meta->album, sizeof(meta->album));
        meta->has_album = (meta->album[0] != '\0');
    }
}

/* ================ 对外接口 ================ */

bool id3_parse(const char *filepath, id3_meta_t *meta)
{
    if (!filepath || !meta) return false;

    FILE *f = fopen(filepath, "rb");
    if (!f) return false;

    bool any = false;

    /* ID3v2 头 */
    unsigned char head[10];
    if (fread(head, 1, 10, f) == 10 && memcmp(head, "ID3", 3) == 0) {
        uint32_t tag_size = ((uint32_t)(head[6] & 0x7F) << 21) |
                            ((uint32_t)(head[7] & 0x7F) << 14) |
                            ((uint32_t)(head[8] & 0x7F) << 7)  |
                            ((uint32_t)(head[9] & 0x7F));
        if (tag_size > 0) {
            parse_id3v2(f, tag_size, meta);
            any = meta->has_title || meta->has_artist || meta->has_album;
        }
    }

    /* ID3v1 (v2 没解析到标题时再查, 避免无谓 IO) */
    if (!meta->has_title && !meta->has_artist) {
        parse_id3v1(f, meta);
        any = any || meta->has_title || meta->has_artist || meta->has_album;
    }

    fclose(f);
    return any;
}
