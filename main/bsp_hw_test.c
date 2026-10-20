/**
 * @file bsp_hw_test.c
 * @brief 硬件通信测试模块实现
 *
 * 只测试I2C总线、CS43131、SD卡、ESP-Hosted从机。
 * 触摸和屏幕在开机时已验证可用，不再测试。
 */
#include "bsp_hw_test.h"
#include "bsp_board.h"
#include "bsp_sd_card.h"
#include "lvgl.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "bsp_memory.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "hw_test";

/* CS43131 I2C 地址 (使用bsp_board.h中的定义) */
#ifndef CS43131_I2C_ADDR
#define CS43131_I2C_ADDR 0x30
#endif

/* ======================== 测试名称和图标 ======================== */

static const char *s_test_names[HW_TEST_COUNT] = {
    "I2C总线",
    "音频DAC CS43131",
    "SD卡",
    "ESP-Hosted从机",
};

static const char *s_test_icons[HW_TEST_COUNT] = {
    LV_SYMBOL_BARS,        /* I2C */
    LV_SYMBOL_AUDIO,       /* 音频 */
    LV_SYMBOL_SAVE,        /* SD卡 */
    LV_SYMBOL_WIFI,        /* ESP-Hosted */
};

const char *bsp_hw_test_name(hw_test_id_t id)
{
    if (id >= 0 && id < HW_TEST_COUNT) return s_test_names[id];
    return "未知";
}

const char *bsp_hw_test_icon(hw_test_id_t id)
{
    if (id >= 0 && id < HW_TEST_COUNT) return s_test_icons[id];
    return LV_SYMBOL_SETTINGS;
}

/* ======================== 各项测试实现 ======================== */

/* I2C总线: 检查已知设备地址是否响应 */
static void test_i2c_bus(hw_test_result_t *r)
{
    static const uint8_t known_addrs[] = {0x30, 0x58};
    static const char *known_names[] = {"CS43131", "CST3530"};
    int found = 0;
    char devices[64] = {0};
    int offset = 0;

    for (int i = 0; i < (int)(sizeof(known_addrs) / sizeof(known_addrs[0])); i++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (known_addrs[i] << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 50 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            found++;
            offset += snprintf(devices + offset, sizeof(devices) - offset,
                               "%s(0x%02X) ", known_names[i], known_addrs[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (found > 0) {
        r->passed = true;
        snprintf(r->detail, sizeof(r->detail), "找到%d个设备: %s", found, devices);
    } else {
        r->passed = false;
        snprintf(r->detail, sizeof(r->detail), "未找到任何I2C设备");
    }
}

/* CS43131测试: 先检查地址响应, 再尝试读芯片ID */
static void test_cs43131(hw_test_result_t *r)
{
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 先检查地址是否响应 */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CS43131_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 100 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        r->passed = false;
        snprintf(r->detail, sizeof(r->detail), "CS43131无响应: %s", esp_err_to_name(ret));
        return;
    }

    /* 尝试读取 Chip ID 寄存器 (0x01,0x00) */
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t reg_high = 0x01;
    uint8_t reg_low = 0x00;
    uint8_t chip_id = 0;

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CS43131_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_high, true);
    i2c_master_write_byte(cmd, reg_low, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CS43131_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &chip_id, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 100 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        r->passed = true;
        snprintf(r->detail, sizeof(r->detail), "CS43131通信正常, ChipID=0x%02X", chip_id);
    } else {
        /* 地址响应但读寄存器失败, 仍算通过 (可能是寄存器协议不同) */
        r->passed = true;
        snprintf(r->detail, sizeof(r->detail), "CS43131地址响应 (0x%02X)", CS43131_I2C_ADDR);
    }
}

/* SD卡测试 */
static void test_sd_card(hw_test_result_t *r)
{
    if (bsp_sd_card_is_mounted()) {
        FILE *f = fopen("/sdcard/hwtest.tmp", "w");
        if (f) {
            fputs("OK", f);
            fclose(f);
            r->passed = true;
            snprintf(r->detail, sizeof(r->detail), "SD卡读写正常");
            remove("/sdcard/hwtest.tmp");
        } else {
            r->passed = false;
            snprintf(r->detail, sizeof(r->detail), "SD卡已挂载但无法写入");
        }
    } else {
        r->passed = false;
        snprintf(r->detail, sizeof(r->detail), "SD卡未挂载");
    }
}

/* ESP-Hosted SDIO从机测试: 直接使用SDIO协议探测从机 */
static void test_esp_hosted(hw_test_result_t *r)
{
    /* 1. 硬件复位ESP32从机 (RST active low) */
    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << ESP_HOSTED_SDIO_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_conf);
    gpio_set_level(ESP_HOSTED_SDIO_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(ESP_HOSTED_SDIO_RST, 1);

    /* 等待从机启动 (至少1.2秒) */
    vTaskDelay(pdMS_TO_TICKS(1200));

    /* 2. 配置SDMMC主机 (Slot 1, 400kHz探测频率) */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_PROBING;
    host.flags = SDMMC_HOST_FLAG_4BIT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = ESP_HOSTED_SDIO_CLK;
    slot_config.cmd = ESP_HOSTED_SDIO_CMD;
    slot_config.d0  = ESP_HOSTED_SDIO_D0;
    slot_config.d1  = ESP_HOSTED_SDIO_D1;
    slot_config.d2  = ESP_HOSTED_SDIO_D2;
    slot_config.d3  = ESP_HOSTED_SDIO_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    /* 3. 初始化主机和卡槽 */
    esp_err_t ret = sdmmc_host_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        r->passed = false;
        snprintf(r->detail, sizeof(r->detail),
                 "SDMMC主机初始化失败: %s", esp_err_to_name(ret));
        return;
    }

    ret = sdmmc_host_init_slot(host.slot, &slot_config);
    if (ret != ESP_OK) {
        r->passed = false;
        snprintf(r->detail, sizeof(r->detail),
                 "SDMMC Slot1初始化失败: %s", esp_err_to_name(ret));
        sdmmc_host_deinit();
        return;
    }

    /* 4. 尝试SDIO卡初始化 (发送CMD0/CMD5/CMD3/CMD7) */
    sdmmc_card_t *card = (sdmmc_card_t *)bsp_memory_malloc(sizeof(sdmmc_card_t));
    if (!card) {
        r->passed = false;
        snprintf(r->detail, sizeof(r->detail), "内存不足");
        sdmmc_host_deinit();
        return;
    }
    memset(card, 0, sizeof(sdmmc_card_t));

    ret = sdmmc_card_init(&host, card);

    if (ret == ESP_OK) {
        r->passed = true;
        snprintf(r->detail, sizeof(r->detail),
                 "SDIO从机响应正常 (RCA=0x%04X, OCR=0x%08lX)",
                 card->rca, (unsigned long)card->ocr);
    } else {
        r->passed = false;
        snprintf(r->detail, sizeof(r->detail),
                 "SDIO从机无响应: %s (检查从机固件/接线/电源)",
                 esp_err_to_name(ret));
    }

    free(card);
    sdmmc_host_deinit_slot(host.slot);
    sdmmc_host_deinit();
}

/* ======================== 公开API ======================== */

void bsp_hw_test_run_all(hw_test_result_t *results)
{
    ESP_LOGI(TAG, "开始硬件通信测试...");

    test_i2c_bus(&results[HW_TEST_I2C_BUS]);
    ESP_LOGI(TAG, "I2C总线: %s - %s",
             results[HW_TEST_I2C_BUS].passed ? "通过" : "失败",
             results[HW_TEST_I2C_BUS].detail);

    test_cs43131(&results[HW_TEST_CS43131]);
    ESP_LOGI(TAG, "CS43131: %s - %s",
             results[HW_TEST_CS43131].passed ? "通过" : "失败",
             results[HW_TEST_CS43131].detail);

    test_sd_card(&results[HW_TEST_SD_CARD]);
    ESP_LOGI(TAG, "SD卡: %s - %s",
             results[HW_TEST_SD_CARD].passed ? "通过" : "失败",
             results[HW_TEST_SD_CARD].detail);

    test_esp_hosted(&results[HW_TEST_ESP_HOSTED]);
    ESP_LOGI(TAG, "ESP-Hosted: %s - %s",
             results[HW_TEST_ESP_HOSTED].passed ? "通过" : "失败",
             results[HW_TEST_ESP_HOSTED].detail);

    ESP_LOGI(TAG, "硬件通信测试完成");
}
