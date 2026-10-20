/**
 * @file lrc_parser.c
 * @brief LRC 歌词文件解析实现
 */

#include "lrc_parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "id3_tags.h"   /* 复用 GBK->UTF-8 归一化 */

/* 读取一行 (含换行符), 返回行长度 */
static int read_line(FILE *f, char *buf, size_t cap)
{
    if (cap == 0) return 0;
    size_t i = 0;
    int c;
    while (i + 1 < cap && (c = fgetc(f)) != EOF) {
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return (int)i;
}

/* 解析一个 [mm:ss.xx] 时间标签 */
static bool parse_tag_time(const char *tag, size_t tag_len, uint32_t *ms_out)
{
    /* tag 形如 "mm:ss.xx" 或 "mm:ss" 或 "mm:ss.xxx" */
    if (tag_len < 4) return false;
    int min = 0, sec = 0, msec = 0;
    const char *colon = NULL;
    for (size_t i = 0; i < tag_len; i++) {
        if (tag[i] == ':') { colon = &tag[i]; break; }
    }
    if (!colon) return false;
    char tmp[16];
    size_t min_len = (size_t)(colon - tag);
    if (min_len == 0 || min_len >= sizeof(tmp)) return false;
    memcpy(tmp, tag, min_len);
    tmp[min_len] = '\0';
    char *endp = NULL;
    min = (int)strtol(tmp, &endp, 10);
    if (endp == tmp) return false;

    /* 秒部分 */
    size_t rest_len = tag_len - min_len - 1;
    if (rest_len < 1 || rest_len >= sizeof(tmp)) return false;
    memcpy(tmp, colon + 1, rest_len);
    tmp[rest_len] = '\0';
    endp = NULL;
    sec = (int)strtol(tmp, &endp, 10);
    if (endp == tmp) return false;
    if (*endp == '.') {
        int digits = (int)strlen(endp + 1);
        if (digits > 3) digits = 3;
        char frac[4] = {0, 0, 0, 0};
        for (int i = 0; i < digits; i++) frac[i] = endp[1 + i];
        msec = atoi(frac);
        /* 按位数归一化到毫秒 */
        if (digits == 1) msec *= 100;
        else if (digits == 2) msec *= 10;
    }

    if (min < 0 || sec < 0 || sec > 59) return false;
    *ms_out = (uint32_t)(min * 60000 + sec * 1000 + msec);
    return true;
}

/* 排序回调 */
static int cmp_line(const void *a, const void *b)
{
    const lrc_line_t *la = (const lrc_line_t *)a;
    const lrc_line_t *lb = (const lrc_line_t *)b;
    if (la->time_ms < lb->time_ms) return -1;
    if (la->time_ms > lb->time_ms) return 1;
    return 0;
}

/* 归一化文本 (UTF-8 直用, 否则 GBK 转换) */
static void norm_lrc_text(const char *src, char *dst, size_t dst_cap)
{
    id3_normalize_text(src, dst, dst_cap);
}

int lrc_load_for_song(const char *mp3_path, lrc_line_t *out_lines, int max_lines)
{
    if (!mp3_path || !out_lines || max_lines <= 0) return 0;

    /* 构建 .lrc 路径: 替换扩展名 */
    char lrc_path[320];
    size_t len = strlen(mp3_path);
    if (len >= sizeof(lrc_path) - 5) return 0;
    /* 找到最后一个 '.' */
    size_t dot = len;
    for (size_t i = len; i > 0; i--) {
        if (mp3_path[i - 1] == '.') { dot = i - 1; break; }
    }
    memcpy(lrc_path, mp3_path, dot);
    strcpy(lrc_path + dot, ".lrc");

    FILE *f = fopen(lrc_path, "r");
    if (!f) return 0;

    int count = 0;
    char line[512];

    while (count < max_lines && read_line(f, line, sizeof(line)) > 0) {
        /* 去掉行尾 \r\n */
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
            line[--l] = '\0';
        }
        if (l == 0) continue;

        /* 元信息行: [ti:...][ar:...] 等, 直接忽略 */
        if (line[0] == '[' && (strncmp(line, "[ti:", 4) == 0 ||
                               strncmp(line, "[ar:", 4) == 0 ||
                               strncmp(line, "[al:", 4) == 0 ||
                               strncmp(line, "[by:", 4) == 0 ||
                               strncmp(line, "[offset:", 8) == 0)) {
            continue;
        }

        /* 解析行首的多个时间标签 [..][..] */
        int added_this_line = 0;
        const char *p = line;
        while (*p == '[') {
            if (count >= max_lines) break; /* 容量保护: 一行多标签不可越界写 */
            const char *close = strchr(p, ']');
            if (!close) break;
            uint32_t ms;
            if (!parse_tag_time(p + 1, (size_t)(close - p - 1), &ms)) {
                /* 不是时间标签, 停止解析该行 */
                break;
            }
            const char *text = close + 1;
            if (*text == '\0') break;   /* 无歌词内容 */

            lrc_line_t *entry = &out_lines[count];
            entry->time_ms = ms;
            norm_lrc_text(text, entry->text, sizeof(entry->text));
            count++;
            added_this_line++;
            p = text;

            /* 一行多个标签时, 后面的标签带同一行文本: "同一行文本" */
            if (*p == '[') {
                /* 继续解析下一个标签, 使用相同的 text 起点 */
                continue;
            }
        }

        if (added_this_line == 0 && count == 0 && l > 0) {
            /* 行首无时间标签, 跳过 (非标准 LRC) */
        }
    }

    fclose(f);

    if (count > 1) {
        qsort(out_lines, (size_t)count, sizeof(lrc_line_t), cmp_line);
    }
    return count;
}

int lrc_find_line(const lrc_line_t *lines, int count, uint32_t ms)
{
    if (!lines || count <= 0) return -1;
    /* 找到最后一条 time_ms <= ms 的行 */
    int idx = -1;
    for (int i = 0; i < count; i++) {
        if (lines[i].time_ms <= ms) idx = i;
        else break;
    }
    return idx;
}
