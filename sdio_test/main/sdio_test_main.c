/**
 * @file sdio_test_main.c
 * @brief ESP32-P4 SDIO接线测试程序
 *
 * 用于验证 ESP32-P4 (HOST) ↔ 外部 ESP32 (SLAVE) 的 SDIO 接线是否正确。
 * 同时测试所有关键外设的连接状态。
 *
 * 接线:
 *   GPIO28 → SDIO CMD
 *   GPIO31 → SDIO CLK
 *   GPIO27 → SDIO D0
 *   GPIO26 → SDIO D1
 *   GPIO30 → SDIO D2
 *   GPIO29 → SDIO D3
 *   GPIO32 → SLAVE_RST
 *
 * 结果通过串口输出, 同时在屏幕上显示。
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_reg.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_io_expander.h"
#include "lvgl.h"
#include "esp_lcd_sh8601.h"

/* ======================== 引脚定义 ======================== */
#define SDIO_CMD_GPIO    28
#define SDIO_CLK_GPIO    31
#define SDIO_D0_GPIO     27
#define SDIO_D1_GPIO     26
#define SDIO_D2_GPIO     30
#define SDIO_D3_GPIO     29
#define SLAVE_RST_GPIO   32

#define I2C_SDA          18
#define I2C_SCL          19
#define TOUCH_INT        22
#define TOUCH_RST        21
#define CS43131_RST      20
#define AMOLED_VCI_EN    53
#define TFT_RST          23

#define MIPI_DSI_LCD_H_RES   262
#define MIPI_DSI_LCD_V_RES   928

/* ======================== 全局变量 ======================== */
static const char *TAG = "SDIO_TEST";

/* LCD面板IO句柄 (用于测试MIPI通信) */
static esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
static esp_lcd_panel_handle_t mipi_panel = NULL;

/* LVGL显示驱动 */
static lv_display_t *lvgl_disp = NULL;

/* 测试结果行 */
#define MAX_LINES 40
static char s_lines[MAX_LINES][80];
static int s_line_count = 0;

/* 添加一行测试结果 */
static void add_line(const char *fmt, ...)
{
    if (s_line_count >= MAX_LINES) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_lines[s_line_count], sizeof(s_lines[0]), fmt, args);
    va_end(args);
    ESP_LOGI(TAG, "%s", s_lines[s_line_count]);
    s_line_count++;
}

/* ======================== SDIO引脚测试 ======================== */

/* 测试1: GPIO直连测试 - 检查SDIO引脚是否能正常拉高拉低 */
static void test_gpio_direct(void)
{
    add_line("=== GPIO直连测试 ===");

    int fail_count = 0;
    int pins[] = {SDIO_CMD_GPIO, SDIO_CLK_GPIO, SDIO_D0_GPIO,
                  SDIO_D1_GPIO, SDIO_D2_GPIO, SDIO_D3_GPIO};
    const char *names[] = {"CMD(28)", "CLK(31)", "D0(27)",
                           "D1(26)", "D2(30)", "D3(29)"};

    for (int i = 0; i < 6; i++) {
        /* 配置为输出 */
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);

        /* 输出高电平 */
        gpio_set_level(pins[i], 1);
        vTaskDelay(pdMS_TO_TICKS(1));
        int high = gpio_get_level(pins[i]);

        /* 输出低电平 */
        gpio_set_level(pins[i], 0);
        vTaskDelay(pdMS_TO_TICKS(1));
        int low = gpio_get_level(pins[i]);

        if (high == 1 && low == 0) {
            add_line("  %s: OK (H=%d, L=%d)", names[i], high, low);
        } else {
            add_line("  %s: FAIL (H=%d, L=%d) 短路?", names[i], high, low);
            fail_count++;
        }
    }

    if (fail_count == 0) {
        add_line("  结果: 全部引脚OK");
    } else {
        add_line("  结果: %d个引脚异常", fail_count);
    }
}

/* 测试2: SLAVE_RST测试 - 拉低复位, 检查从机响应 */
static void test_slave_reset(void)
{
    add_line("=== SLAVE_RST测试 ===");

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << SLAVE_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* 拉低复位 */
    gpio_set_level(SLAVE_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    int low = gpio_get_level(SLAVE_RST_GPIO);

    /* 释放复位 */
    gpio_set_level(SLAVE_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    int high = gpio_get_level(SLAVE_RST_GPIO);

    add_line("  RST低电平: %d (期望0)", low);
    add_line("  RST高电平: %d (期望1)", high);

    if (low == 0 && high == 1) {
        add_line("  结果: RST引脚正常");
    } else {
        add_line("  结果: RST引脚异常, 检查接线");
    }

    /* 切回输入模式, 看从机是否会拉低 */
    gpio_set_direction(SLAVE_RST_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(SLAVE_RST_GPIO, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(100));
    int input_level = gpio_get_level(SLAVE_RST_GPIO);
    add_line("  RST输入模式: %d (1=从机未拉低)", input_level);
}

/* 测试3: SDIO上拉检测 - 从机正常工作时D0应有上拉 */
static void test_sdio_pullup(void)
{
    add_line("=== SDIO上拉检测 ===");

    /* 配置SDIO数据线为输入+上拉 */
    int pins[] = {SDIO_D0_GPIO, SDIO_D1_GPIO, SDIO_D2_GPIO, SDIO_D3_GPIO};
    const char *names[] = {"D0(27)", "D1(26)", "D2(30)", "D3(29)"};

    for (int i = 0; i < 4; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    for (int i = 0; i < 4; i++) {
        int level = gpio_get_level(pins[i]);
        add_line("  %s: %d (1=上拉正常)", names[i], level);
    }

    /* CMD线检测 */
    gpio_config_t cmd_conf = {
        .pin_bit_mask = 1ULL << SDIO_CMD_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cmd_conf);
    vTaskDelay(pdMS_TO_TICKS(10));
    add_line("  CMD(28): %d", gpio_get_level(SDIO_CMD_GPIO));
}

/* 测试4: I2C总线扫描 */
static void test_i2c_scan(void)
{
    add_line("=== I2C总线扫描 ===");

    /* 初始化I2C主机 */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    i2c_param_config(I2C_NUM_0, &conf);
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);

    int found = 0;
    char devices[128] = {0};
    int offset = 0;

    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            found++;
            offset += snprintf(devices + offset, sizeof(devices) - offset,
                               "0x%02X ", addr);
        }
    }

    if (found > 0) {
        add_line("  找到%d个设备: %s", found, devices);

        /* 检查关键设备 */
        /* CST3530 触摸 @0x5A */
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (0x5A << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 50 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        add_line("  CST3530(0x5A): %s", ret == ESP_OK ? "OK" : "无响应");

        /* CS43131 DAC @0x18 */
        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (0x18 << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 50 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        add_line("  CS43131(0x18): %s", ret == ESP_OK ? "OK" : "无响应");
    } else {
        add_line("  未找到任何I2C设备!");
        add_line("  请检查SDA/SCL接线");
    }
}

/* 测试5: SDIO CMD线信号检测 */
static void test_sdio_cmd_signal(void)
{
    add_line("=== SDIO CMD信号检测 ===");

    /* 先复位从机 */
    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << SLAVE_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_conf);
    gpio_set_level(SLAVE_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(SLAVE_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* 尝试发送SDIO CMD0 (GO_IDLE_STATE) */
    /* SDIO CMD格式: 起始位(0) + 传输位(1) + CMD索引(6bit) + 参数(32bit) + CRC(7bit) + 停止位(1) */
    add_line("  从机已复位, 等待1秒...");
    add_line("  CMD线状态: %d", gpio_get_level(SDIO_CMD_GPIO));

    /* 配置CMD为输出, 尝试手动发CMD0 */
    gpio_config_t cmd_conf = {
        .pin_bit_mask = 1ULL << SDIO_CMD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cmd_conf);

    /* 发送CMD0: 01 000000 00000000 00000000 00000000 00000000 CRC + 1 */
    /* 简化: 只检测CMD线是否能切换 */
    gpio_set_level(SDIO_CMD_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    int low = gpio_get_level(SDIO_CMD_GPIO);
    gpio_set_level(SDIO_CMD_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    int high = gpio_get_level(SDIO_CMD_GPIO);

    add_line("  CMD输出: H=%d, L=%d", high, low);

    /* 切回输入, 检测从机是否在CMD线上响应 */
    gpio_set_direction(SDIO_CMD_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(SDIO_CMD_GPIO, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(100));

    int response = gpio_get_level(SDIO_CMD_GPIO);
    add_line("  CMD输入(从机响应): %d", response);

    if (high == 1 && low == 0) {
        add_line("  结果: CMD线电气正常");
        if (response == 1) {
            add_line("  从机可能已启动(上拉正常)");
        } else {
            add_line("  从机可能未响应(CMD被拉低)");
        }
    } else {
        add_line("  结果: CMD线异常, 检查接线");
    }
}

/* ======================== LCD初始化 (简化版) ======================== */

static void lcd_init(void)
{
    /* LDO使能 */
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy);

    /* VCI使能 */
    gpio_config_t vci_conf = {
        .pin_bit_mask = 1ULL << AMOLED_VCI_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&vci_conf);
    gpio_set_level(AMOLED_VCI_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* TFT RST */
    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << TFT_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_conf);
    gpio_set_level(TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* MIPI DSI 初始化 */
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .phy_lcd_pixel_clk = 16000000,
        .lane_bit_rate_mbps = 800,
    };
    esp_lcd_new_dsi_bus(&bus_cfg, &mipi_dsi_bus);

    /* DBI配置 */
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_new_panel_dbi_io(mipi_dsi_bus, &dbi_config, &mipi_dbi_io);

    /* SH8601/ICNA3312 面板 */
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_new_panel_sh8601(mipi_dbi_io, &panel_config, &mipi_panel);
    esp_lcd_panel_reset(mipi_panel);
    esp_lcd_panel_init(mipi_panel);

    /* DPI显示 */
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 16,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = 262,
            .v_size = 928,
            .hsync_back_porch = 0,
            .hsync_pulse_width = 0,
            .hsync_front_porch = 0,
            .vsync_back_porch = 0,
            .vsync_pulse_width = 0,
            .vsync_front_porch = 0,
        },
        .flags.use_dma2d = true,
    };
    esp_lcd_new_panel_dpi(mipi_dsi_bus, &dpi_config, &mipi_panel);
    esp_lcd_panel_init(mipi_panel);
}

/* ======================== LVGL显示 ======================== */

static lv_obj_t *s_text_area = NULL;

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    lv_draw_sw_rgb565_swap(px_map, 262 * (area->y2 - area->y1 + 1));
    esp_lcd_panel_draw_bitmap(mipi_panel, 0, area->y1, 262, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);
}

static void lvgl_init(void)
{
    lv_init();
    lvgl_disp = lv_display_create(262, 928);
    lv_display_set_flush_cb(lvgl_disp, lvgl_flush_cb);

    /* 双缓冲 (PSRAM) */
    size_t buf_size = 262 * 928 * 2;
    void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(lvgl_disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_DIRECT);
}

static void display_results(void)
{
    if (s_text_area == NULL) return;

    /* 构建完整文本 */
    char full_text[3200] = {0};
    int offset = 0;
    for (int i = 0; i < s_line_count; i++) {
        offset += snprintf(full_text + offset, sizeof(full_text) - offset,
                           "%s\n", s_lines[i]);
    }
    lv_textarea_set_text(s_text_area, full_text);
}

static void create_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP32-P4 SDIO接线测试");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00ff88), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* 结果文本区域 */
    s_text_area = lv_textarea_create(scr);
    lv_obj_set_size(s_text_area, 252, 840);
    lv_obj_align(s_text_area, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(s_text_area, lv_color_hex(0x111122), 0);
    lv_obj_set_style_bg_opa(s_text_area, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_text_area, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_text_area, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_border_width(s_text_area, 1, 0);
    lv_obj_set_style_border_color(s_text_area, lv_color_hex(0x333355), 0);
    lv_textarea_set_text(s_text_area, "测试中, 请稍候...");
}

/* ======================== LVGL任务 ======================== */

static void lvgl_task(void *arg)
{
    create_ui();

    /* 运行所有测试 */
    test_gpio_direct();
    display_results();
    vTaskDelay(pdMS_TO_TICKS(500));

    test_slave_reset();
    display_results();
    vTaskDelay(pdMS_TO_TICKS(500));

    test_sdio_pullup();
    display_results();
    vTaskDelay(pdMS_TO_TICKS(500));

    test_i2c_scan();
    display_results();
    vTaskDelay(pdMS_TO_TICKS(500));

    test_sdio_cmd_signal();
    display_results();

    add_line("");
    add_line("=== 测试完成 ===");
    add_line("请查看上方结果");
    add_line("如果所有测试OK, 说明接线正确");
    add_line("可在主项目中启用ESP-Hosted");
    display_results();

    while (1) {
        uint32_t time_next = lv_timer_handler();
        if (time_next < 5) time_next = 5;
        vTaskDelay(pdMS_TO_TICKS(time_next));
    }
}

/* ======================== 主入口 ======================== */

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-P4 SDIO接线测试程序启动");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "HOST (P4) → SLAVE (ESP32) SDIO接线:");
    ESP_LOGI(TAG, "  GPIO28 → SDIO CMD");
    ESP_LOGI(TAG, "  GPIO31 → SDIO CLK");
    ESP_LOGI(TAG, "  GPIO27 → SDIO D0");
    ESP_LOGI(TAG, "  GPIO26 → SDIO D1");
    ESP_LOGI(TAG, "  GPIO30 → SDIO D2");
    ESP_LOGI(TAG, "  GPIO29 → SDIO D3");
    ESP_LOGI(TAG, "  GPIO32 → SLAVE_RST");
    ESP_LOGI(TAG, "========================================");

    /* 初始化LCD */
    lcd_init();
    ESP_LOGI(TAG, "LCD初始化完成");

    /* 初始化LVGL */
    lvgl_init();
    ESP_LOGI(TAG, "LVGL初始化完成");

    /* 启动LVGL任务 (含测试) */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 10, NULL, 0);

    ESP_LOGI(TAG, "测试任务已启动");
}
