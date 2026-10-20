/*******************************************************************************
 * SD卡文件管理器 UI 模块实现 - LVGL 9.x
 *
 * 屏幕: 262×928 (竖屏) RGB565
 * 布局 (从上到下):
 *   标题栏(50) | 路径栏(30) | 文件列表(818, 可滚动) | 状态栏(30)
 ******************************************************************************/

#include "ui_file_manager.h"
#include "bsp_board.h"
#include "bsp_theme.h"
#include "ui_gesture.h"
#include "esp_attr.h"
#include <sys/stat.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>

/* 返回桌面 (由桌面模块提供) */
extern void ui_desktop_show(void);

/* 前置声明 */
static void go_up(void);

/* ======================== 文件名编码兑底 (UTF-8 / GBK) ======================== */
/*
 * 修改 sdkconfig 后 FATFS API 默认返回 UTF-8. 但有些用户用 Windows 把 SD 卡格式化
 * 为 FAT32/exFAT 后写入 GBK 编码的中文文件名 (且没有 LFN), 此时 opendir/readdir
 * 返回的字节流是 GBK 而不是 UTF-8, LVGL 会显示乱码.
 *
 * 这里在每次复制文件名时做一次启发式判断:
 *   - 如果整个字符串是合法 UTF-8, 直接用 (字节就是 LVGL 期望的编码).
 *   - 否则按 GBK 解码并转写为 UTF-8 (限制: 仅覆盖 GB2312 一二级汉字 ~6763 个,
 *     之外的字符原样输出).
 */

/* 单字节 ASCII -> Unicode code point (直通) */
static uint32_t gbk_decode_ascii(uint8_t b)
{
    return (uint32_t)b;
}

/* GBK 双字节区 0x8140-0xFEFE. 返回 Unicode code point, 失败返回 0.
 * 注: 完整 GBK 表太大 (~20KB), 这里只覆盖常用汉字 (~6763), 用紧凑区间偏移.
 * 对生僻字返回 0 -> 调用方原样输出 GBK 字节 (LVGL 仍可能显示问号, 但至少 ASCII 正常). */
extern uint32_t gbk_to_unicode_lookup(uint16_t gbk_code);

/* 判断字符串是不是合法 UTF-8 (不含未配对的代理/截断序列) */
static bool looks_like_utf8(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint8_t b = *p;
        if (b < 0x80) { p++; continue; }                      /* ASCII */
        if ((b & 0xE0) == 0xC0) {                              /* 2 字节 */
            if ((p[1] & 0xC0) != 0x80) return false;
            if ((b & 0x1E) == 0) return false;                /* overlong */
            p += 2;
        } else if ((b & 0xF0) == 0xE0) {                       /* 3 字节 */
            if ((p[1] & 0xC0) != 0x80) return false;
            if ((p[2] & 0xC0) != 0x80) return false;
            uint32_t cp = ((b & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            if (cp < 0x800) return false;                     /* overlong */
            if (cp >= 0xD800 && cp <= 0xDFFF) return false;    /* surrogate */
            p += 3;
        } else if ((b & 0xF8) == 0xF0) {                       /* 4 字节 */
            if ((p[1] & 0xC0) != 0x80) return false;
            if ((p[2] & 0xC0) != 0x80) return false;
            if ((p[3] & 0xC0) != 0x80) return false;
            uint32_t cp = ((b & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
                          ((p[2] & 0x3F) << 6)  |  (p[3] & 0x3F);
            if (cp < 0x10000) return false;
            p += 4;
        } else {
            return false;                                       /* 非法首字节 */
        }
    }
    return true;
}

/* 写一个 Unicode code point 到 UTF-8 输出 buffer. 返回写入字节数. */
static int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

/* 把可能是 GBK 的字节流转写为 UTF-8. 失败/不可映射的字节原样输出 (LVGL 至少能渲染 ASCII). */
static size_t normalize_name_to_utf8(const char *src, char *dst, size_t dst_cap)
{
    if (src == NULL || dst == NULL || dst_cap == 0) return 0;

    /* 已经是合法 UTF-8, 直接拷贝 (这是常见情况: LFN 存的就是 UTF-8/UTF-16 经 FatFs 解码). */
    if (looks_like_utf8(src)) {
        size_t n = strnlen(src, dst_cap - 1);
        memcpy(dst, src, n);
        dst[n] = '\0';
        return n;
    }

    /* 按 GBK 解码 + 转 UTF-8 */
    const uint8_t *p = (const uint8_t *)src;
    size_t out = 0;
    while (*p && out + 4 < dst_cap) {
        uint8_t b = *p;
        if (b < 0x80) {
            uint32_t cp = gbk_decode_ascii(b);
            out += utf8_encode(cp, dst + out);
            p++;
            continue;
        }
        /* GBK 双字节: 0x81-0xFE + 0x40-0x7E | 0x80-0xFE */
        if (b >= 0x81 && b <= 0xFE && p[1] != 0) {
            uint8_t b2 = p[1];
            if ((b2 >= 0x40 && b2 <= 0x7E) || (b2 >= 0x80 && b2 <= 0xFE)) {
                uint16_t gbk_code = (uint16_t)((b << 8) | b2);
                uint32_t cp = gbk_to_unicode_lookup(gbk_code);
                if (cp != 0) {
                    out += utf8_encode(cp, dst + out);
                    p += 2;
                    continue;
                }
            }
        }
        /* 不可识别: 原样输出 */
        dst[out++] = (char)b;
        p++;
    }
    dst[out] = '\0';
    return out;
}

/* 侧滑返回 (与返回按钮逻辑一致) */
static void swipe_back(void)
{
    go_up();
}

/* ======================== 颜色定义 (使用主题系统) ======================== */
static inline lv_color_t theme_bg(void)     { return bsp_theme_colors()->bg; }
static inline lv_color_t theme_card(void)   { return bsp_theme_colors()->card; }
static inline lv_color_t theme_accent(void) { return bsp_theme_colors()->accent; }

#define COLOR_BG        theme_bg()
#define COLOR_CARD      theme_card()
#define COLOR_ACCENT    theme_accent()
#define COLOR_WHITE     lv_color_hex(0xffffff)
#define COLOR_GRAY      lv_color_hex(0x888888)
#define COLOR_DIR       lv_color_hex(0xFFD700)   /* 文件夹图标 黄色 */
#define COLOR_MUSIC     lv_color_hex(0x00FF00)   /* 音乐图标 绿色 */

/* ======================== 屏幕尺寸 / 区域 ======================== */
#define SCREEN_W   262
#define SCREEN_H   928

#define TITLE_H    50
#define PATH_H     30
#define STATUS_H   30
#define LIST_Y     (TITLE_H + PATH_H)                 /* 80 */
#define LIST_H     (SCREEN_H - LIST_Y - STATUS_H)     /* 818 */

/* ======================== 常量 ======================== */
#define MAX_FILES     128
#define MAX_NAME_LEN  256
#define PATH_LEN      256

/* 目录扫描结果状态 */
#define SCAN_OK         0    /* 成功 */
#define SCAN_NO_SD     (-1)   /* SD卡未挂载 */
#define SCAN_OPEN_FAIL (-2)   /* 无法打开目录 */

/* ======================== 文件信息结构 ======================== */
typedef struct {
    char    name[MAX_NAME_LEN];   /* 文件名 */
    int     is_dir;               /* 1=文件夹, 0=文件 */
    int     is_music;             /* 1=.wav 音乐文件 */
    size_t  size;                 /* 文件大小(字节) */
} file_entry_t;

/* ======================== 静态状态 ======================== */
static char           current_path[PATH_LEN] = "/sdcard";   /* 当前路径 */
static file_entry_t   s_files[MAX_FILES] EXT_RAM_BSS_ATTR;  /* 当前目录文件列表 (~34KB, 放 PSRAM) */
static int            s_file_count = 0;                      /* 文件数量 */

/* ======================== UI 对象引用 ======================== */
static lv_obj_t *g_screen     = NULL;
static lv_obj_t *g_path_lbl   = NULL;
static lv_obj_t *g_file_list  = NULL;
static lv_obj_t *g_status_lbl = NULL;

/* ======================== 前置声明 ======================== */
static void cb_back(lv_event_t *e);
static void cb_item_click(lv_event_t *e);

static lv_obj_t *make_container(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                lv_coord_t w, lv_coord_t h);
static void create_title_bar(lv_obj_t *p);
static void create_path_bar(lv_obj_t *p);
static void create_file_list(lv_obj_t *p);
static void create_status_bar(lv_obj_t *p);

static void  format_size(size_t bytes, char *buf, size_t len);
static int   scan_directory(void);
static void  sort_files(void);
static void  populate_list(int scan_status);
static void  add_file_item(int index);
static void  add_up_item(void);
static void  enter_dir(const char *name);
static void  go_up(void);
static void  play_music_file(const char *filename);

/* ========================================================================
 * 辅助函数
 * ======================================================================== */

/* 格式化文件大小: B / KB / MB */
static void format_size(size_t bytes, char *buf, size_t len)
{
    if (bytes < 1024) {
        snprintf(buf, len, "%dB", (int)bytes);
    } else if (bytes < 1048576) {
        snprintf(buf, len, "%.1fKB", bytes / 1024.0);
    } else {
        snprintf(buf, len, "%.1fMB", bytes / 1048576.0);
    }
}

/* 创建透明无边框容器 (用于各区域) */
static lv_obj_t *make_container(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

/* ========================================================================
 * 路径操作
 * ======================================================================== */

/* 进入子目录 */
static void enter_dir(const char *name)
{
    size_t len = strlen(current_path);
    int n = snprintf(current_path + len, PATH_LEN - len, "/%s", name);
    if (n < 0 || (size_t)n >= PATH_LEN - len) {
        /* 路径过长, 回滚 */
        current_path[len] = '\0';
        return;
    }
    ui_file_manager_refresh();
}

/* 返回上级目录, 已在根目录则返回桌面 */
static void go_up(void)
{
    if (strcmp(current_path, "/sdcard") == 0) {
        ui_desktop_show();
        return;
    }
    char *last = strrchr(current_path, '/');
    if (last != NULL && last != current_path) {
        *last = '\0';
    } else {
        /* 兜底: 回到根目录 */
        strcpy(current_path, "/sdcard");
    }
    ui_file_manager_refresh();
}

/* ========================================================================
 * 目录扫描
 * ======================================================================== */

/* 扫描当前目录, 填充 s_files 数组, 返回扫描状态 */
static int scan_directory(void)
{
    s_file_count = 0;

    if (!bsp_sd_card_is_mounted()) {
        return SCAN_NO_SD;
    }

    DIR *dir = opendir(current_path);
    if (dir == NULL) {
        return SCAN_OPEN_FAIL;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_file_count < MAX_FILES) {
        /* 跳过 "." 和 ".." */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        file_entry_t *fe = &s_files[s_file_count];

        /* 文件名: 如果是合法 UTF-8 直接用, 否则按 GBK 解码并转写为 UTF-8,
         * 避免 LVGL 把 GBK 字节按 Latin-1 渲染成乱码 (Windows 写入的 SD 卡常见).
         *
         * 注意: 后面拼接路径也必须用 fe->name (因为文件系统对 d_name/fe->name
         * 的字节内容一致). */
        normalize_name_to_utf8(ent->d_name, fe->name, MAX_NAME_LEN);

        /* 类型 */
        fe->is_dir = (ent->d_type == DT_DIR) ? 1 : 0;

        /* 文件大小 (用 fe->name 拼路径, 保证与目录项字节一致) */
        fe->size = 0;
        if (!fe->is_dir) {
            char filepath[PATH_LEN + MAX_NAME_LEN + 2];
            snprintf(filepath, sizeof(filepath), "%s/%s", current_path, fe->name);
            struct stat st;
            if (stat(filepath, &st) == 0) {
                fe->size = (size_t)st.st_size;
            }
        }

        /* 是否为音乐文件 (.wav/.mp3) - 用 fe->name 检测扩展名 */
        fe->is_music = 0;
        if (!fe->is_dir) {
            const char *ext = strrchr(fe->name, '.');
            if (ext != NULL && (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".mp3") == 0)) {
                fe->is_music = 1;
            }
        }

        s_file_count++;
    }
    closedir(dir);

    return SCAN_OK;
}

/* 排序比较: 文件夹优先, 再按名称字母序(不区分大小写) */
static int cmp_file(const void *a, const void *b)
{
    const file_entry_t *fa = (const file_entry_t *)a;
    const file_entry_t *fb = (const file_entry_t *)b;
    if (fa->is_dir != fb->is_dir) {
        return fb->is_dir - fa->is_dir;  /* 文件夹在前 */
    }
    return strcasecmp(fa->name, fb->name);
}

static void sort_files(void)
{
    if (s_file_count > 1) {
        qsort(s_files, s_file_count, sizeof(file_entry_t), cmp_file);
    }
}

/* ========================================================================
 * 音乐播放
 * ======================================================================== */

/* 扫描当前目录的音乐文件并按文件名查找索引播放 */
static void play_music_file(const char *filename)
{
    audio_player_scan_music(current_path);
    int count = audio_player_get_music_count();
    for (int i = 0; i < count; i++) {
        const char *name = audio_player_get_music_name(i);
        if (name != NULL && strcmp(name, filename) == 0) {
            audio_player_play_index(i);
            return;
        }
    }
}

/* ========================================================================
 * UI 区域创建
 * ======================================================================== */

/* 1. 标题栏 (高50): 返回按钮 + "文件管理" */
static void create_title_bar(lv_obj_t *p)
{
    lv_obj_t *bar = make_container(p, 0, 0, SCREEN_W, TITLE_H);
    lv_obj_set_style_bg_color(bar, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    /* 返回按钮 (40x40, 透明背景) */
    lv_obj_t *back_btn = lv_btn_create(bar);
    lv_obj_set_size(back_btn, 40, 40);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(back_btn, cb_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(back_lbl, COLOR_WHITE, 0);
    lv_obj_center(back_lbl);

    /* 标题 "文件管理" */
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "文件管理");
    lv_obj_set_style_text_font(title, &esp_2, 0);
    lv_obj_set_style_text_color(title, COLOR_WHITE, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 48, 0);
}

/* 2. 路径栏 (高30): 文件夹图标 + 当前路径 */
static void create_path_bar(lv_obj_t *p)
{
    lv_obj_t *bar = make_container(p, 0, TITLE_H, SCREEN_W, PATH_H);
    lv_obj_set_style_bg_color(bar, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(bar, 8, 0);
    lv_obj_set_style_pad_right(bar, 8, 0);
    lv_obj_set_style_pad_column(bar, 4, 0);

    /* 文件夹图标 */
    lv_obj_t *icon = lv_label_create(bar);
    lv_label_set_text(icon, LV_SYMBOL_DIRECTORY);
    lv_obj_set_style_text_font(icon, &esp_2, 0);
    lv_obj_set_style_text_color(icon, COLOR_DIR, 0);

    /* 当前路径 (过长循环滚动) */
    g_path_lbl = lv_label_create(bar);
    lv_label_set_text(g_path_lbl, current_path);
    lv_obj_set_style_text_font(g_path_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_path_lbl, COLOR_GRAY, 0);
    lv_label_set_long_mode(g_path_lbl, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_flex_grow(g_path_lbl, 1);
    lv_obj_set_style_min_width(g_path_lbl, 0, 0);
}

/* 3. 文件列表 (可滚动, 填充剩余空间) */
static void create_file_list(lv_obj_t *p)
{
    g_file_list = lv_list_create(p);
    lv_obj_set_pos(g_file_list, 0, LIST_Y);
    lv_obj_set_size(g_file_list, SCREEN_W, LIST_H);
    lv_obj_set_style_bg_color(g_file_list, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(g_file_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_file_list, 0, 0);
    lv_obj_set_style_radius(g_file_list, 0, 0);
    lv_obj_set_style_pad_all(g_file_list, 4, 0);
    lv_obj_set_style_pad_row(g_file_list, 4, 0);
}

/* 4. 底部状态栏 (高30): "共X个项目" */
static void create_status_bar(lv_obj_t *p)
{
    lv_obj_t *bar = make_container(p, 0, SCREEN_H - STATUS_H, SCREEN_W, STATUS_H);
    lv_obj_set_style_bg_color(bar, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    g_status_lbl = lv_label_create(bar);
    lv_label_set_text(g_status_lbl, "共0个项目");
    lv_obj_set_style_text_font(g_status_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(g_status_lbl, COLOR_GRAY, 0);
    lv_obj_align(g_status_lbl, LV_ALIGN_LEFT_MID, 8, 0);
}

/* ========================================================================
 * 列表填充
 * ======================================================================== */

/* 添加一个文件项到列表 */
static void add_file_item(int index)
{
    file_entry_t *fe = &s_files[index];

    /* 图标与右侧文本 */
    const char *icon;
    lv_color_t  icon_color;
    const char *right_text;
    char size_buf[24];

    if (fe->is_dir) {
        icon        = LV_SYMBOL_DIRECTORY;
        icon_color  = COLOR_DIR;
        right_text  = ">";
    } else if (fe->is_music) {
        icon        = LV_SYMBOL_AUDIO;
        icon_color  = COLOR_MUSIC;
        format_size(fe->size, size_buf, sizeof(size_buf));
        right_text  = size_buf;
    } else {
        icon        = LV_SYMBOL_FILE;
        icon_color  = COLOR_GRAY;
        format_size(fe->size, size_buf, sizeof(size_buf));
        right_text  = size_buf;
    }

    /* 列表按钮 (空内容, 手动添加子对象以精确控制图标/文字颜色) */
    lv_obj_t *btn = lv_list_add_button(g_file_list, NULL, NULL);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, 50);
    lv_obj_set_style_bg_color(btn, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_all(btn, 8, 0);
    lv_obj_set_style_pad_column(btn, 8, 0);
    /* 按下态高亮 (LVGL9: 选择器 = 部分 | 状态) */
    lv_obj_set_style_bg_color(btn, COLOR_ACCENT, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 图标标签 */
    lv_obj_t *icon_lbl = lv_label_create(btn);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_set_style_text_font(icon_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(icon_lbl, icon_color, 0);

    /* 文件名标签 (弹性填充, 过长省略号) */
    lv_obj_t *name_lbl = lv_label_create(btn);
    lv_label_set_text(name_lbl, fe->name);
    lv_obj_set_style_text_font(name_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(name_lbl, COLOR_WHITE, 0);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(name_lbl, 1);
    lv_obj_set_style_min_width(name_lbl, 0, 0);

    /* 右侧标签 (文件大小 或 ">") */
    lv_obj_t *right_lbl = lv_label_create(btn);
    lv_label_set_text(right_lbl, right_text);
    lv_obj_set_style_text_font(right_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(right_lbl, COLOR_GRAY, 0);

    /* 点击回调, 传递文件索引 */
    lv_obj_add_event_cb(btn, cb_item_click, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);
}

/* 添加"返回上一级"条目 (列表顶部, 非根目录时显示) */
static void add_up_item(void)
{
    lv_obj_t *btn = lv_list_add_button(g_file_list, NULL, NULL);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, 50);
    lv_obj_set_style_bg_color(btn, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_all(btn, 8, 0);
    lv_obj_set_style_pad_column(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, COLOR_ACCENT, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 返回图标 */
    lv_obj_t *icon_lbl = lv_label_create(btn);
    lv_label_set_text(icon_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(icon_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(icon_lbl, COLOR_DIR, 0);

    /* 文字 */
    lv_obj_t *name_lbl = lv_label_create(btn);
    lv_label_set_text(name_lbl, "返回上一级");
    lv_obj_set_style_text_font(name_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(name_lbl, COLOR_WHITE, 0);
    lv_obj_set_flex_grow(name_lbl, 1);
    lv_obj_set_style_min_width(name_lbl, 0, 0);

    /* 右侧提示 */
    lv_obj_t *right_lbl = lv_label_create(btn);
    lv_label_set_text(right_lbl, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(right_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(right_lbl, COLOR_GRAY, 0);

    /* 索引 -1 表示返回上一级 */
    lv_obj_add_event_cb(btn, cb_item_click, LV_EVENT_CLICKED,
                        (void *)(intptr_t)(-1));
}

/* 根据扫描结果填充列表 */
static void populate_list(int scan_status)
{
    lv_obj_clean(g_file_list);

    const char *hint_text = NULL;

    if (scan_status == SCAN_NO_SD) {
        hint_text = "SD卡未挂载";
    } else if (scan_status == SCAN_OPEN_FAIL) {
        hint_text = "无法打开目录";
    }

    if (hint_text != NULL) {
        lv_obj_t *hint = lv_list_add_text(g_file_list, hint_text);
        lv_obj_set_style_text_font(hint, &esp_2, 0);
        lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    if (s_file_count <= 0) {
        lv_obj_t *hint = lv_list_add_text(g_file_list, "空文件夹");
        lv_obj_set_style_text_font(hint, &esp_2, 0);
        lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    /* 非根目录时, 列表顶部添加"返回上一级" */
    if (strcmp(current_path, "/sdcard") != 0) {
        add_up_item();
    }

    for (int i = 0; i < s_file_count; i++) {
        add_file_item(i);
    }
}

/* ========================================================================
 * 事件回调
 * ======================================================================== */

/* 返回按钮: 返回上级目录 / 根目录返回桌面 */
static void cb_back(lv_event_t *e)
{
    (void)e;
    go_up();
}

/* 列表项点击: 文件夹则进入, .wav 则播放, -1 返回上一级 */
static void cb_item_click(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index == -1) {
        go_up();
        return;
    }
    if (index < 0 || index >= s_file_count) {
        return;
    }

    file_entry_t *fe = &s_files[index];
    if (fe->is_dir) {
        enter_dir(fe->name);
    } else if (fe->is_music) {
        play_music_file(fe->name);
    }
    /* 其他文件类型暂不处理 */
}

/* ========================================================================
 * 公开 API
 * ======================================================================== */

void ui_file_manager_show(void)
{
    if (g_screen == NULL) {
        g_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(g_screen, COLOR_BG, 0);
        lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(g_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(g_screen, 0, 0);

        create_title_bar(g_screen);
        create_path_bar(g_screen);
        create_file_list(g_screen);
        create_status_bar(g_screen);

        /* 添加侧滑返回手势 */
        ui_edge_swipe_add(g_screen, swipe_back);
    }

    /* 直接加载，避免动画冲突 */
    lv_screen_load(g_screen);
    ui_file_manager_refresh();
}

void ui_file_manager_refresh(void)
{
    if (g_file_list == NULL) {
        return;
    }

    /* 更新路径栏 */
    lv_label_set_text(g_path_lbl, current_path);

    /* 扫描并排序 */
    int status = scan_directory();
    sort_files();

    /* 填充列表 */
    populate_list(status);

    /* 更新状态栏 */
    char status_buf[48];
    snprintf(status_buf, sizeof(status_buf), "共%d个项目", s_file_count);
    lv_label_set_text(g_status_lbl, status_buf);
}
