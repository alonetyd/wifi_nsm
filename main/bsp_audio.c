/**
 * @file bsp_audio.c
 * @brief CS43131 Hi-Fi DAC 驱动 + I2S 音频输出
 *
 * CS43131 通过 I2C 配置寄存器，通过 I2S 接收 PCM 音频数据。
 * I2C 总线与触摸驱动共用（I2C_NUM_0），无需重复初始化。
 */

#include "bsp_audio.h"
#include "bsp_board.h"
#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp_sd_card.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "bsp_audio";

/* I2S 通道句柄 */
static i2s_chan_handle_t s_tx_handle = NULL;

/* 当前音量 0-100 */
static uint8_t s_volume = 70;

/* 当前采样率 */
static uint32_t s_sample_rate = 44100;

/* 防抖保存定时器 */
#define VOLUME_SAVE_DEBOUNCE_MS  500
#define VOLUME_I2C_THROTTLE_MS  120
#define VOLUME_I2C_RETRY_MS     100

/* NVS 存储 (主存储) */
#define NVS_NAMESPACE_VOL    "volume"
#define NVS_KEY_VOL          "vol"

static esp_timer_handle_t s_vol_save_timer = NULL;
static bool s_vol_save_pending = false;

static esp_timer_handle_t s_vol_i2c_timer = NULL;
static bool s_vol_i2c_pending = false;
static uint8_t s_last_vol_reg = 0xFF;

/* ================ CS43131 I2C 寄存器写入 ================ */

/**
 * @brief 向 CS43131 写入一个寄存器
 * @param reg  16位寄存器地址
 * @param val  写入值
 */
/**
 * @brief 向 CS43131 写入一个寄存器（带 I2C 总线争用重试）
 * @param reg  16位寄存器地址
 * @param val  写入值
 */
static esp_err_t cs43131_write_reg(uint16_t reg, uint8_t val)
{
    esp_err_t ret = ESP_FAIL;
    for (int retry = 0; retry < 3; retry++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (CS43131_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
        i2c_master_write_byte(cmd, reg & 0xFF, true);
        i2c_master_write_byte(cmd, val, true);
        i2c_master_stop(cmd);
        ret = i2c_master_cmd_begin(CS43131_I2C_HOST, cmd, 200 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CS43131 写寄存器 0x%04X 失败: %s (已重试3次)",
                  reg, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t cs43131_write_reg_fast(uint16_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CS43131_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
    i2c_master_write_byte(cmd, reg & 0xFF, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(CS43131_I2C_HOST, cmd, 20 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void volume_i2c_timer_cb(void *arg)
{
    (void)arg;
    if (!s_vol_i2c_pending) return;
    s_vol_i2c_pending = false;

    uint8_t vol_reg = (s_volume * 0x78) / 100;
    if (vol_reg == s_last_vol_reg) return;

    esp_err_t ret_l = cs43131_write_reg_fast(0x40, vol_reg);
    esp_err_t ret_r = cs43131_write_reg_fast(0x3F, vol_reg);
    if (ret_l == ESP_OK && ret_r == ESP_OK) {
        s_last_vol_reg = vol_reg;
    } else {
        /* I2C 总线被触摸驱动占用时写入会失败: 保持 pending 并重试,
         * 否则这次调节被静默丢弃 (表现为"拖了没反应"), 直到下次拖动 */
        s_vol_i2c_pending = true;
        esp_timer_start_once(s_vol_i2c_timer, VOLUME_I2C_RETRY_MS * 1000);
    }
}

/* ================ CS43131 初始化序列 ================ */

/**
 * @brief 初始化 CS43131 DAC
 * 配置为 I2S 从机模式，44.1kHz / 16bit / stereo
 */
static esp_err_t cs43131_init(void)
{
    esp_err_t ret;

    /* 1. 硬件复位: RST 拉低 10ms，拉高，等待 200ms */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << CS43131_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);

    gpio_set_level(CS43131_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CS43131_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 2. 设置系统时钟: RCO 模式 */
    ret = cs43131_write_reg(0x07, 0x06);
    if (ret != ESP_OK) return ret;

    /* 3. 设置采样率: 44.1kHz */
    ret = cs43131_write_reg(0x08, 0x01);
    if (ret != ESP_OK) return ret;

    /* 4. 设置位深: 16bit ASP */
    ret = cs43131_write_reg(0x09, 0x05);
    if (ret != ESP_OK) return ret;

    /* 5. ASP I2S 配置 */
    /* LRCK 周期低8位 = 63 (64个SCK周期/帧) */
    cs43131_write_reg(0x1E, 0x3F);
    /* LRCK 周期高8位 */
    cs43131_write_reg(0x1F, 0x00);
    /* LRCK 高电平时间低8位 = 31 */
    cs43131_write_reg(0x1C, 0x1F);
    /* LRCK 高电平时间高8位 */
    cs43131_write_reg(0x1D, 0x00);
    /* I2S 格式, delay=2 */
    cs43131_write_reg(0x21, 0x0A);
    /* CH1 位置 */
    cs43131_write_reg(0x22, 0x00);
    /* CH2 位置 = 32 */
    cs43131_write_reg(0x23, 0x20);
    /* CH1 16bit + enable */
    cs43131_write_reg(0x24, 0x06);
    /* CH2 16bit + enable */
    cs43131_write_reg(0x25, 0x0E);
    /* SCK 上升沿采样, LRCK 低有效, slave */
    cs43131_write_reg(0x20, 0x0C);

    /* 6. PCM 路径 */
    /* PCM 使能 + mute */
    cs43131_write_reg(0x41, 0xA8);
    /* 音量 A, 0dB */
    cs43131_write_reg(0x40, 0x78);
    /* 音量 B, 0dB */
    cs43131_write_reg(0x3F, 0x78);
    /* PCM 滤波选项 */
    cs43131_write_reg(0x3E, 0x02);

    /* 7. HP 输出 */
    /* HP 输出控制 */
    cs43131_write_reg(0x44, 0x30);
    /* Class H 控制 */
    cs43131_write_reg(0x43, 0x1E);

    /* 8. 电源上电 */
    cs43131_write_reg(0x02, 0x00);

    /* 等待上电稳定 */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 9. 设置音量 */
    uint8_t vol_reg = (s_volume * 0x78) / 100;
    cs43131_write_reg(0x40, vol_reg);
    cs43131_write_reg(0x3F, vol_reg);
    s_last_vol_reg = vol_reg;

    /* 10. 取消静音 */
    cs43131_write_reg(0x41, 0x28);

    ESP_LOGI(TAG, "CS43131 初始化完成");
    return ESP_OK;
}

/* ================ I2S 初始化 ================ */

static esp_err_t i2s_bus_init(void)
{
    /* 创建 I2S 通道 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 配置 I2S 标准模式 */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = AUDIO_I2S_BCK_IO,
            .ws   = AUDIO_I2S_WS_IO,
            .dout = AUDIO_I2S_DO_IO,
            .din  = GPIO_NUM_NC,
            .mclk = GPIO_NUM_NC,
        },
    };

    ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化 I2S 标准模式失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 启用 I2S 通道 */
    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启用 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2S 初始化完成, 采样率=%lu", s_sample_rate);
    return ESP_OK;
}

/* ================ 音量持久化 (防抖保存) ================ */

static esp_err_t volume_nvs_save(uint8_t vol)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_VOL, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, NVS_KEY_VOL, vol);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static bool volume_nvs_load(uint8_t *out)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_VOL, NVS_READONLY, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_get_u8(handle, NVS_KEY_VOL, out);
    nvs_close(handle);
    return err == ESP_OK;
}

static esp_err_t write_volume_file(void)
{
    if (!bsp_sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    FILE *f = fopen(VOLUME_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "无法写入音量文件: %s (errno=%d)", VOLUME_FILE, errno);
        return ESP_FAIL;
    }
    fprintf(f, "volume=%d\n", s_volume);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return ESP_OK;
}

static void volume_save_timer_cb(void *arg)
{
    (void)arg;
    if (!s_vol_save_pending) return;

    esp_err_t nvs_err = volume_nvs_save(s_volume);
    if (nvs_err != ESP_OK) {
        esp_timer_start_once(s_vol_save_timer, VOLUME_SAVE_DEBOUNCE_MS * 1000);
        return;
    }
    s_vol_save_pending = false;
    ESP_LOGI(TAG, "音量已保存: %d", s_volume);

    if (bsp_sd_card_is_mounted()) {
        write_volume_file();
    }
}

/* 从SD卡或NVS读取音量 */
static void volume_load(void)
{
    uint8_t nvs_val = 0;
    if (volume_nvs_load(&nvs_val) && nvs_val <= 100) {
        s_volume = nvs_val;
        ESP_LOGI(TAG, "从NVS读取音量: %d", s_volume);
        return;
    }

    if (!bsp_sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD卡未挂载，使用默认音量: %d", VOLUME_DEFAULT);
        s_volume = VOLUME_DEFAULT;
        return;
    }

    FILE *f = fopen(VOLUME_FILE, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "volume=", 7) == 0) {
                int val = atoi(line + 7);
                if (val >= 0 && val <= 100) {
                    s_volume = (uint8_t)val;
                    ESP_LOGI(TAG, "从SD卡读取音量: %d", s_volume);
                }
            }
        }
        fclose(f);
    } else {
        ESP_LOGW(TAG, "音量配置文件不存在，使用默认值: %d", VOLUME_DEFAULT);
        s_volume = VOLUME_DEFAULT;
    }
}

/* ================ 公共接口 ================ */

esp_err_t bsp_audio_init(void)
{
    esp_err_t ret;

    /* 从NVS/SD卡读取音量 */
    volume_load();

    /* 先初始化 I2S，产生时钟信号，CS43131 作为从机需要时钟 */
    ret = i2s_bus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    /* 初始化 CS43131 DAC */
    ret = cs43131_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CS43131 初始化失败");
        return ret;
    }

    ESP_LOGI(TAG, "音频子系统初始化完成, 音量=%d", s_volume);
    return ESP_OK;
}

esp_err_t bsp_audio_set_volume(uint8_t vol)
{
    if (vol > 100) {
        vol = 100;
    }
    s_volume = vol;

    s_vol_i2c_pending = true;
    if (s_vol_i2c_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = volume_i2c_timer_cb,
            .arg = NULL,
            .name = "vol_i2c",
        };
        esp_timer_create(&args, &s_vol_i2c_timer);
    }
    esp_timer_stop(s_vol_i2c_timer);
    esp_timer_start_once(s_vol_i2c_timer, VOLUME_I2C_THROTTLE_MS * 1000);

    s_vol_save_pending = true;
    if (s_vol_save_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = volume_save_timer_cb,
            .arg = NULL,
            .name = "volume_save",
        };
        esp_timer_create(&args, &s_vol_save_timer);
    }
    esp_timer_stop(s_vol_save_timer);
    esp_timer_start_once(s_vol_save_timer, VOLUME_SAVE_DEBOUNCE_MS * 1000);

    return ESP_OK;
}

uint8_t bsp_audio_get_volume(void)
{
    return s_volume;
}

esp_err_t bsp_audio_write(const uint8_t *data, size_t len, size_t *bytes_written)
{
    if (s_tx_handle == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2s_channel_write(s_tx_handle, data, len, bytes_written, portMAX_DELAY);
}

esp_err_t bsp_audio_set_sample_rate(uint32_t rate)
{
    if (s_tx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (rate == s_sample_rate) {
        return ESP_OK;
    }

    /* 先禁用通道 */
    esp_err_t ret = i2s_channel_disable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "禁用 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 重新配置时钟 */
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    ret = i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "重配置 I2S 时钟失败: %s", esp_err_to_name(ret));
        /* 恢复使能 */
        i2s_channel_enable(s_tx_handle);
        return ret;
    }

    /* 重新使能通道 */
    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "重新使能 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 更新 CS43131 采样率 */
    uint8_t sr_val;
    if (rate <= 48000) {
        /* 根据常用采样率设置 */
        if (rate == 44100) {
            sr_val = 0x01;
        } else if (rate == 48000) {
            sr_val = 0x02;
        } else if (rate == 32000) {
            sr_val = 0x00;
        } else if (rate == 96000) {
            sr_val = 0x03;
        } else {
            sr_val = 0x01;  /* 默认44.1kHz */
        }
    } else {
        sr_val = 0x03;  /* 96kHz */
    }
    cs43131_write_reg(0x08, sr_val);

    s_sample_rate = rate;
    ESP_LOGI(TAG, "采样率切换为 %luHz", rate);
    return ESP_OK;
}
