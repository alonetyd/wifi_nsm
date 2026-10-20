/**
 * @file lrc_parser.h
 * @brief LRC 歌词文件解析
 *
 * 支持: [mm:ss] / [mm:ss.xx] / [mm:ss.xxx] 时间标签, 一行多标签,
 *       元信息标签 [ti:][ar:][al:][by:][offset:], 编码 UTF-8 / GBK。
 */
#ifndef LRC_PARSER_H
#define LRC_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LRC_TEXT_MAX   96
#define LRC_MAX_LINES  300

typedef struct {
    uint32_t time_ms;       /* 该行歌词起始时间 (毫秒) */
    char     text[LRC_TEXT_MAX];
} lrc_line_t;

/**
 * @brief 从 mp3 文件路径推导同名 .lrc 并解析
 * @param mp3_path  mp3 文件路径 (如 /sdcard/music/xxx.mp3)
 * @param out_lines 输出行数组 (由调用方分配, 建议 LRC_MAX_LINES 个)
 * @param max_lines 数组容量
 * @return 实际解析到的歌词行数 (0 = 无歌词)
 */
int lrc_load_for_song(const char *mp3_path, lrc_line_t *out_lines, int max_lines);

/**
 * @brief 查找指定毫秒时刻应高亮的歌词行索引
 * @param lines 歌词行数组
 * @param count 行数
 * @param ms    当前播放毫秒
 * @return 行索引; 无匹配返回 -1
 */
int lrc_find_line(const lrc_line_t *lines, int count, uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* LRC_PARSER_H */
