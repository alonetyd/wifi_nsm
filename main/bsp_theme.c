/**
 * @file bsp_theme.c
 * @brief 主题颜色管理模块实现
 */
#include "bsp_theme.h"
#include "bsp_board.h"
#include "bsp_sd_card.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "bsp_theme";

#define THEME_FILE  "/sdcard/theme.cfg"
#define THEME_SAVE_DEBOUNCE_MS  500

/* ======================== 主题颜色定义 ======================== */
/* 所有背景色使用纯黑(0x000000)以发挥AMOLED屏幕省电优势 */

static const theme_colors_t s_themes[THEME_COUNT] = {
    /* 深蓝 (默认) */
    [THEME_DARK_BLUE] = {
        .bg        = LV_COLOR_MAKE(0x00, 0x00, 0x00),  /* 纯黑 AMOLED */
        .card      = LV_COLOR_MAKE(0x16, 0x21, 0x3e),
        .primary   = LV_COLOR_MAKE(0x0f, 0x34, 0x60),
        .accent    = LV_COLOR_MAKE(0xe9, 0x45, 0x60),
        .watch_top = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .watch_bot = LV_COLOR_MAKE(0x0f, 0x34, 0x60),
    },
    /* 暗夜黑 */
    [THEME_DARK_NIGHT] = {
        .bg        = LV_COLOR_MAKE(0x00, 0x00, 0x00),  /* 纯黑 AMOLED */
        .card      = LV_COLOR_MAKE(0x1a, 0x1a, 0x1a),
        .primary   = LV_COLOR_MAKE(0x33, 0x33, 0x33),
        .accent    = LV_COLOR_MAKE(0x00, 0xbf, 0xa5),
        .watch_top = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .watch_bot = LV_COLOR_MAKE(0x1a, 0x1a, 0x1a),
    },
    /* 森林绿 */
    [THEME_FOREST] = {
        .bg        = LV_COLOR_MAKE(0x00, 0x00, 0x00),  /* 纯黑 AMOLED */
        .card      = LV_COLOR_MAKE(0x1a, 0x2e, 0x1a),
        .primary   = LV_COLOR_MAKE(0x2e, 0x5a, 0x2e),
        .accent    = LV_COLOR_MAKE(0x76, 0xff, 0x03),
        .watch_top = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .watch_bot = LV_COLOR_MAKE(0x1a, 0x3a, 0x1a),
    },
    /* 紫罗兰 */
    [THEME_VIOLET] = {
        .bg        = LV_COLOR_MAKE(0x00, 0x00, 0x00),  /* 纯黑 AMOLED */
        .card      = LV_COLOR_MAKE(0x1a, 0x13, 0x30),
        .primary   = LV_COLOR_MAKE(0x31, 0x1b, 0x92),
        .accent    = LV_COLOR_MAKE(0xe0, 0x40, 0xfb),
        .watch_top = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .watch_bot = LV_COLOR_MAKE(0x2a, 0x15, 0x55),
    },
    /* 日落橙 */
    [THEME_SUNSET] = {
        .bg        = LV_COLOR_MAKE(0x00, 0x00, 0x00),  /* 纯黑 AMOLED */
        .card      = LV_COLOR_MAKE(0x2e, 0x1a, 0x1a),
        .primary   = LV_COLOR_MAKE(0x5a, 0x2e, 0x2e),
        .accent    = LV_COLOR_MAKE(0xff, 0x6f, 0x00),
        .watch_top = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .watch_bot = LV_COLOR_MAKE(0x3a, 0x1e, 0x15),
    },
    /* 海洋蓝 */
    [THEME_OCEAN] = {
        .bg        = LV_COLOR_MAKE(0x00, 0x00, 0x00),  /* 纯黑 AMOLED */
        .card      = LV_COLOR_MAKE(0x0a, 0x2e, 0x4a),
        .primary   = LV_COLOR_MAKE(0x0d, 0x47, 0xa1),
        .accent    = LV_COLOR_MAKE(0x00, 0xe5, 0xff),
        .watch_top = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .watch_bot = LV_COLOR_MAKE(0x0a, 0x3a, 0x5a),
    },
};

static const char *s_theme_names[THEME_COUNT] = {
    "深蓝",
    "暗夜黑",
    "森林绿",
    "紫罗兰",
    "日落橙",
    "海洋蓝",
};

/* ======================== 模块状态 ======================== */

static theme_id_t s_current_theme = THEME_DARK_BLUE;
static esp_timer_handle_t s_save_timer = NULL;
static bool s_save_pending = false;

/* 防抖保存回调 */
static void theme_save_timer_cb(void *arg)
{
    (void)arg;
    if (!s_save_pending) return;
    s_save_pending = false;

    if (!bsp_sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD卡未挂载，无法保存主题");
        return;
    }

    FILE *f = fopen(THEME_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "无法创建主题配置文件");
        return;
    }

    fprintf(f, "theme=%d\n", (int)s_current_theme);
    fflush(f);
    fclose(f);

    ESP_LOGI(TAG, "主题已保存: %s", s_theme_names[s_current_theme]);
}

void bsp_theme_init(void)
{
    if (bsp_sd_card_is_mounted()) {
        FILE *f = fopen(THEME_FILE, "r");
        if (f) {
            char line[64];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "theme=", 6) == 0) {
                    int val = atoi(line + 6);
                    if (val >= 0 && val < THEME_COUNT) {
                        s_current_theme = (theme_id_t)val;
                        ESP_LOGI(TAG, "从SD卡读取主题: %s",
                                 s_theme_names[s_current_theme]);
                    }
                }
            }
            fclose(f);
        } else {
            ESP_LOGW(TAG, "主题配置文件不存在，使用默认主题: %s",
                     s_theme_names[s_current_theme]);
        }
    } else {
        ESP_LOGW(TAG, "SD卡未挂载，使用默认主题");
    }
}

theme_id_t bsp_theme_get(void)
{
    return s_current_theme;
}

void bsp_theme_set(theme_id_t id)
{
    if (id < 0 || id >= THEME_COUNT) return;
    if (id == s_current_theme) return;

    s_current_theme = id;
    ESP_LOGI(TAG, "主题切换为: %s", s_theme_names[id]);

    /* 防抖保存 */
    if (s_save_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = theme_save_timer_cb,
            .arg = NULL,
            .name = "theme_save",
        };
        esp_timer_create(&args, &s_save_timer);
    }

    s_save_pending = true;
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, THEME_SAVE_DEBOUNCE_MS * 1000);
}

const theme_colors_t *bsp_theme_colors(void)
{
    return &s_themes[s_current_theme];
}

const char *bsp_theme_name(theme_id_t id)
{
    if (id >= 0 && id < THEME_COUNT) return s_theme_names[id];
    return "未知";
}

lv_color_t bsp_theme_preview_color(theme_id_t id)
{
    if (id >= 0 && id < THEME_COUNT) {
        return s_themes[id].accent;
    }
    return lv_color_hex(0xFFFFFF);
}
