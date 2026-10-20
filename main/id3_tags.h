/**
 * @file id3_tags.h
 * @brief MP3 ID3 标签解析 (ID3v1 / ID3v2.2 / v2.3 / v2.4)
 *
 * 提取: 曲名(TIT2/TT2/TAG)、艺术家(TPE1/TP1)、专辑(TALB/TAL)、
 *       APIC 封面图片 (可选, 提取到 /sdcard/music/.cache/<name>.jpg)。
 * 编码: 支持 ISO-8859-1 / UTF-16(LE/BE, 含 BOM) / UTF-16BE / UTF-8。
 *       对于声明为 ISO-8859-1 但实为 GBK 的中文标签, 自动用 GBK 表转 UTF-8。
 */
#ifndef ID3_TAGS_H
#define ID3_TAGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ID3_TITLE_MAX   128
#define ID3_ARTIST_MAX  64
#define ID3_ALBUM_MAX   64

typedef struct {
    char title[ID3_TITLE_MAX];
    char artist[ID3_ARTIST_MAX];
    char album[ID3_ALBUM_MAX];
    bool has_title;
    bool has_artist;
    bool has_album;
} id3_meta_t;

/**
 * @brief 解析 MP3 文件的 ID3 标签
 * @param filepath MP3 文件完整路径
 * @param meta     输出元数据 (调用前 memset 为 0)
 * @return true 解析到至少一项元数据
 */
bool id3_parse(const char *filepath, id3_meta_t *meta);

/**
 * @brief 把可能是 GBK 的字节流转写为 UTF-8 (与 ui_file_manager 相同策略)
 * @param src     输入字节流 (可含 GBK 双字节 / ASCII)
 * @param dst     输出 UTF-8 缓冲
 * @param dst_cap 输出容量
 * @return 写入的字节数 (不含结尾 '\0')
 */
size_t id3_normalize_text(const char *src, char *dst, size_t dst_cap);

#ifdef __cplusplus
}
#endif

#endif /* ID3_TAGS_H */
