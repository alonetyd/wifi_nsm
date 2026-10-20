/**
 * @file bsp_theme.h
 * @brief 主题颜色管理模块
 *
 * 提供多种颜色主题, 选择后保存到SD卡, 断电不丢失。
 * 所有UI模块通过 bsp_theme_colors() 获取当前主题颜色。
 */
#ifndef BSP_THEME_H
#define BSP_THEME_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 主题枚举 */
typedef enum {
    THEME_DARK_BLUE = 0,    /* 深蓝 (默认) */
    THEME_DARK_NIGHT,       /* 暗夜黑 */
    THEME_FOREST,           /* 森林绿 */
    THEME_VIOLET,           /* 紫罗兰 */
    THEME_SUNSET,           /* 日落橙 */
    THEME_OCEAN,            /* 海洋蓝 */
    THEME_COUNT
} theme_id_t;

/* 主题颜色集合 */
typedef struct {
    lv_color_t bg;          /* 背景色 */
    lv_color_t card;        /* 卡片色 */
    lv_color_t primary;     /* 主色调 (按下态) */
    lv_color_t accent;      /* 强调色 (按钮、高亮) */
    lv_color_t watch_top;   /* 表盘渐变顶 */
    lv_color_t watch_bot;   /* 表盘渐变底 */
} theme_colors_t;

/**
 * @brief 初始化主题模块, 从SD卡读取上次选择的主题
 */
void bsp_theme_init(void);

/**
 * @brief 获取当前主题ID
 */
theme_id_t bsp_theme_get(void);

/**
 * @brief 设置主题并保存到SD卡
 * @param id 主题ID
 */
void bsp_theme_set(theme_id_t id);

/**
 * @brief 获取当前主题颜色集
 * @return 指向当前主题颜色的常量指针
 */
const theme_colors_t *bsp_theme_colors(void);

/**
 * @brief 获取主题名称
 */
const char *bsp_theme_name(theme_id_t id);

/**
 * @brief 获取主题预览色 (用于UI中的色块预览)
 */
lv_color_t bsp_theme_preview_color(theme_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* BSP_THEME_H */
