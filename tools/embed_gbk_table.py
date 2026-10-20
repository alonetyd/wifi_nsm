#!/usr/bin/env python3
"""
读取 gbk_table_data.txt (生成器输出) 并嵌入到 main/gbk_to_unicode.c.

用法: python tools/embed_gbk_table.py
"""

import os
import sys

THIS = os.path.dirname(os.path.abspath(__file__))
TABLE_FILE = os.path.join(THIS, "gbk_table_data.txt")
HEADER = """/**
 * @file gbk_to_unicode.c
 * @brief GB2312 双字节字符 -> Unicode 码点 转换表
 *
 * 覆盖范围:
 *   - GB2312 一级汉字 3755 个 (按拼音排序, GBK 0xB0A1-0xD7FA), 覆盖中国大陆
 *     日常用字 99% 以上.
 *   - GB2312 二级汉字 6763 个 (按部首排序, GBK 0xD8A1-0xF7FE).
 *   - GB2312 符号区 (0xA1A1-0xA9FE 区间常用符号: 全角数字/字母/标点/部首).
 *
 * 不覆盖:
 *   - GBK 扩展汉字 (0xFE00-0xFEFE 第三四字节部分) - 生僻字/异体字
 *   - 用户自定义区 (0xAAA1-0xAFFE) - 罕用
 *
 * 数据来源: tools/gen_gbk_table.py 用 Python 标准库 GB2312 codec 提取,
 *           经 tools/embed_gbk_table.py 嵌入本文件. 表大小: ~15 KB Flash (rodata).
 *
 * 调用方约定 (ui_file_manager.c):
 *   - 已经检测到字符串不是合法 UTF-8 -> 调用本表按 GBK 解码
 *   - 找不到的字符返回 0 -> 调用方把原字节原样复制
 */

#include <stdint.h>
#include <stdbool.h>

"""

FOOTER = """
uint32_t gbk_to_unicode_lookup(uint16_t gbk_code)
{
    if (gbk_code < 0x8140 || gbk_code > 0xFEFE) return 0;
    uint8_t hi = (uint8_t)(gbk_code >> 8);
    uint8_t lo = (uint8_t)(gbk_code & 0xFF);
    if (hi < 0xA1 || hi > 0xF7) return 0;
    int cell;
    if (lo >= 0xA1 && lo <= 0xFE) cell = lo - 0xA1;
    else return 0;
    int row = hi - 0xA1;
    int off = gbk_offset[row];
    if (off < 0) return 0;
    if (cell >= gbk_size[row]) return 0;
    uint16_t idx = (uint16_t)(off + cell);
    if (idx >= UNICODE_TABLE_SIZE) return 0;
    return unicode_table[idx];
}
"""

def main():
    with open(TABLE_FILE, "r", encoding="ascii") as f:
        table_src = f.read()

    out = HEADER + table_src + FOOTER

    out_path = os.path.normpath(os.path.join(THIS, "..", "main", "gbk_to_unicode.c"))
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(out)
    print("wrote:", out_path)

if __name__ == "__main__":
    main()