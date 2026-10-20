/**
 * @file bsp_brightness.c
 * @brief LCD亮度控制模块实现
 *
 * 通过MIPI DBI接口发送LCD命令0x51设置背光亮度（0-255）。
 *
 * 持久化策略 (重构):
 *   - 主存储: NVS flash。断电可靠保存, 与SD卡状态无关。
 *   - 次存储: SD卡文件 /sdcard/brightness.cfg (尽力写入, 兼容旧版本)。
 *     早期版本反复出现"无法创建亮度配置文件 /sdcard/brightness.cfg"
 *     (SD卡上 fopen 失败, 具体 FATFS 错误由 errno 打印), 而 NVS
 *     不依赖 SD 卡, 因此亮度一定能保存, 不再受 SD 卡文件系统影响。
 *   - 使用 esp_timer 防抖机制, 滑块停止移动500ms后才保存。
 */
#include "bsp_brightness.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "bsp_sd_card.h"
#include <math.h>
#include "soc/mipi_dsi_host_struct.h"

/* 引用main.c中定义的MIPI DBI IO句柄 */
extern esp_lcd_panel_io_handle_t mipi_dbi_io;

static const char *TAG = "bsp_brightness";

/* LCD亮度控制命令 */
#define LCD_CMD_BRIGHTNESS      0x51

/* 防抖保存定时器 */
#define BRIGHTNESS_SAVE_DEBOUNCE_MS  500
#define BRIGHTNESS_MIPI_THROTTLE_MS  40
#define BRIGHTNESS_MIPI_RETRY_MS     60

/* NVS 存储 (主存储, 断电可靠保存) */
#define NVS_NAMESPACE       "brightness"
#define NVS_KEY_LEVEL       "level"

/*
 * Gamma LUT: 将滑块线性值映射到 LCD 寄存器值, 使感知亮度变化均匀.
 * AMOLED 的 0x51 寄存器值与发光电流近似线性, 而人眼感知亮度 ∝ 亮度^0.33,
 * 因此需要 gamma > 1 的曲线补偿, 令 register = slider^gamma / 255^(gamma-1).
 * gamma=2.0 为 AMOLED 常用值; 可根据实际观感在 1.5~2.5 间微调.
 */
#define BRIGHTNESS_GAMMA    2.0f
static uint8_t s_brightness_lut[256];
static bool s_lut_ready = false;

static uint8_t s_brightness = BRIGHTNESS_DEFAULT;
static esp_timer_handle_t s_save_timer = NULL;
static bool s_save_pending = false;

static esp_timer_handle_t s_mipi_timer = NULL;
static bool s_mipi_pending = false;
static uint8_t s_last_reg_val = 0xFF;

static void init_brightness_lut(void)
{
    for (int i = 0; i < 256; i++) {
        float t = (float)i / 255.0f;
        float v = roundf(powf(t, BRIGHTNESS_GAMMA) * 255.0f);
        if (v > 255.0f) v = 255.0f;
        if (v < 0.0f)   v = 0.0f;
        s_brightness_lut[i] = (uint8_t)v;
    }
    s_brightness_lut[0]   = 0;
    s_brightness_lut[255] = 255;
    s_lut_ready = true;
}

static int parse_brightness_file(FILE *f)
{
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "brightness=", 11) == 0) return atoi(p + 11);
        if (*p >= '0' && *p <= '9') return atoi(p);
    }
    return -1;
}

/* ======================== NVS 读写 (主存储) ======================== */

static esp_err_t brightness_nvs_save(uint8_t level)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS打开失败 (%s): %s", NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(handle, NVS_KEY_LEVEL, level);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS写入失败: %s", esp_err_to_name(err));
    }
    return err;
}

static bool brightness_nvs_load(uint8_t *out)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_get_u8(handle, NVS_KEY_LEVEL, out);
    nvs_close(handle);
    return err == ESP_OK;
}

/* ======================== SD卡写入 (次存储, 尽力而为) ========================
 * 直接写入亮度配置文件.
 * 不使用 "临时文件+rename" 方案: 主题模块 (bsp_theme.c) 使用直接写入
 * 在当前 FATFS/SD 环境验证可正常工作, 而 rename 在此文件系统上可能
 * 因为目标已存在或跨目录等原因失败.
 * 写入后重新打开校验, 确保内容真正落盘.
 * 注意: 失败不影响断电保存 (主存储是 NVS), 故只警告一次. */
static esp_err_t write_brightness_file(void)
{
    if (!bsp_sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(BRIGHTNESS_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "无法创建亮度配置文件: %s (errno=%d %s)",
                 BRIGHTNESS_FILE, errno, strerror(errno));
        return ESP_FAIL;
    }

    fprintf(f, "brightness=%u\n", (unsigned)s_brightness);
    if (fflush(f) != 0) {
        ESP_LOGE(TAG, "fflush 亮度配置失败 (errno=%d)", errno);
        fclose(f);
        return ESP_FAIL;
    }
    fsync(fileno(f));
    fclose(f);

    /* 读回校验, 确认文件确实写入成功 */
    FILE *rf = fopen(BRIGHTNESS_FILE, "r");
    if (rf) {
        int val = parse_brightness_file(rf);
        fclose(rf);
        if (val != (int)s_brightness) {
            ESP_LOGE(TAG, "亮度配置校验失败: 写入=%d 读回=%d", s_brightness, val);
            return ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "亮度配置读回失败");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* MIPI 节流回调: 每40ms最多写一次0x51寄存器 */
static void brightness_mipi_timer_cb(void *arg)
{
    (void)arg;
    if (!s_mipi_pending) return;
    s_mipi_pending = false;

    if (mipi_dbi_io == NULL) return;
    uint8_t reg_val = s_lut_ready ? s_brightness_lut[s_brightness] : s_brightness;
    if (reg_val == s_last_reg_val) return;

    esp_err_t ret = esp_lcd_panel_io_tx_param(mipi_dbi_io, LCD_CMD_BRIGHTNESS, &reg_val, 1);
    if (ret == ESP_OK) {
        s_last_reg_val = reg_val;
    } else {
        /* DSI 视频模式下的 LP 命令偶发被丢 (P4 已知行为), 保持 pending
         * 自动重试, 否则这次调节被静默丢弃 (表现为"拖了没反应") */
        s_mipi_pending = true;
        esp_timer_start_once(s_mipi_timer, BRIGHTNESS_MIPI_RETRY_MS * 1000);
    }
}

/* 防抖保存回调: NVS 为主 (必保), SD 卡尽力写入 */
static void brightness_save_timer_cb(void *arg)
{
    (void)arg;
    if (!s_save_pending) return;

    esp_err_t nvs_err = brightness_nvs_save(s_brightness);
    if (nvs_err != ESP_OK) {
        /* NVS 写入失败才重试 */
        esp_timer_start_once(s_save_timer, BRIGHTNESS_SAVE_DEBOUNCE_MS * 1000);
        return;
    }
    s_save_pending = false;
    ESP_LOGI(TAG, "亮度已保存: %d", s_brightness);

    /* SD卡尽力写入 (失败不重试, 不影响主存储) */
    if (bsp_sd_card_is_mounted()) {
        write_brightness_file();
    }
}

esp_err_t bsp_brightness_init(void)
{
    /* 关键修复: MIPI DSI 视频模式下允许发送 LP 命令.
     * IDF 的 DPI 驱动初始化时从不配置 dpi_lp_cmd_tim 寄存器,
     * 其默认值为 0, 会导致视频模式启动后 VACT 内外的 LP 命令
     * (如亮度命令 0x51) 包长上限为 0 而被丢弃.
     * 这解释了"开机初始化时 0x51 有效(视频未启动), 运行后调节无效".
     * 这里显式放宽包长限制 (0x20 = 32 字节, 短包命令足够). */
    MIPI_DSI_HOST.dpi_lp_cmd_tim.invact_lpcmd_time = 0x20;
    MIPI_DSI_HOST.dpi_lp_cmd_tim.outvact_lpcmd_time = 0x20;

    /* 优先从 NVS 读取 (断电可靠保存) */
    uint8_t nvs_val = 0;
    if (brightness_nvs_load(&nvs_val) &&
        nvs_val >= BRIGHTNESS_MIN && nvs_val <= BRIGHTNESS_MAX) {
        s_brightness = nvs_val;
        ESP_LOGI(TAG, "从NVS读取亮度: %d", s_brightness);
    } else {
        /* NVS 无记录时, 兼容旧版本: 从 SD 卡文件读取 (容错处理) */
        if (bsp_sd_card_is_mounted()) {
            FILE *f = fopen(BRIGHTNESS_FILE, "r");
            if (f) {
                int val = parse_brightness_file(f);
                fclose(f);
                if (val >= BRIGHTNESS_MIN && val <= BRIGHTNESS_MAX) {
                    s_brightness = (uint8_t)val;
                    ESP_LOGI(TAG, "从SD卡读取亮度: %d", s_brightness);
                } else {
                    ESP_LOGW(TAG, "亮度配置内容无效(%d)，使用默认值: %d", val, BRIGHTNESS_DEFAULT);
                }
            } else {
                ESP_LOGW(TAG, "亮度配置文件不存在，使用默认值: %d", BRIGHTNESS_DEFAULT);
            }
        } else {
            ESP_LOGW(TAG, "SD卡未挂载，使用默认亮度: %d", BRIGHTNESS_DEFAULT);
        }
    }

    /* 应用亮度到LCD (经过 gamma LUT 映射) */
    if (mipi_dbi_io == NULL) {
        ESP_LOGE(TAG, "MIPI DBI IO未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    init_brightness_lut();
    uint8_t reg_val = s_brightness_lut[s_brightness];
    s_last_reg_val = reg_val;
    esp_err_t ret = esp_lcd_panel_io_tx_param(mipi_dbi_io, LCD_CMD_BRIGHTNESS, &reg_val, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置亮度失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "亮度初始化完成，当前亮度: %d", s_brightness);
    return ESP_OK;
}

esp_err_t bsp_brightness_set(uint8_t level)
{
    if (level < BRIGHTNESS_MIN) {
        level = BRIGHTNESS_MIN;
    }

    s_brightness = level;

    s_mipi_pending = true;
    if (s_mipi_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = brightness_mipi_timer_cb,
            .arg = NULL,
            .name = "bright_mipi",
        };
        esp_timer_create(&args, &s_mipi_timer);
    }
    esp_timer_stop(s_mipi_timer);
    esp_timer_start_once(s_mipi_timer, BRIGHTNESS_MIPI_THROTTLE_MS * 1000);

    s_save_pending = true;
    if (s_save_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = brightness_save_timer_cb,
            .arg = NULL,
            .name = "brightness_save",
        };
        esp_timer_create(&args, &s_save_timer);
    }
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, BRIGHTNESS_SAVE_DEBOUNCE_MS * 1000);

    return ESP_OK;
}

/**
 * @brief 直接设置亮度寄存器 (不限制最小值, 不持久化)
 *
 * 用于睡眠定时熄屏 (0) / 唤醒恢复等临时场景. 值 0-255 原样发送.
 * 注意: 不更新 s_brightness, 避免影响持久化亮度.
 */
esp_err_t bsp_brightness_set_raw(uint8_t level)
{
    if (mipi_dbi_io == NULL) {
        ESP_LOGE(TAG, "MIPI DBI IO未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = esp_lcd_panel_io_tx_param(mipi_dbi_io, LCD_CMD_BRIGHTNESS, &level, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "发送亮度命令失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "设置亮度(raw): %d", level);
    return ESP_OK;
}

uint8_t bsp_brightness_get(void)
{
    return s_brightness;
}

esp_err_t bsp_brightness_save(void)
{
    /* 立即保存: NVS 为主, SD 卡尽力而为 */
    esp_err_t nvs_err = brightness_nvs_save(s_brightness);
    if (nvs_err != ESP_OK) {
        return nvs_err;
    }

    s_save_pending = false;
    if (s_save_timer) esp_timer_stop(s_save_timer);

    esp_err_t sd_err = write_brightness_file();
    ESP_LOGI(TAG, "亮度已保存: %d (SD卡:%s)", s_brightness,
             sd_err == ESP_OK ? "ok" : "跳过");
    return ESP_OK;
}
