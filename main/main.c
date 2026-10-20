#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "driver/i2c.h"

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co6300.h"
#include "esp_lcd_touch_cst3530.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "esp_hosted.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 模块头文件 */
#include "bsp_board.h"
#include "bsp_sd_card.h"
#include "bsp_brightness.h"
#include "bsp_theme.h"
#include "bsp_audio.h"
#include "audio_player.h"
#include "bsp_wifi.h"
#include "bsp_bt.h"
#include "bsp_memory.h"
#include "bsp_wireless_mgr.h"
#include "ui_wireless_bridge.h"
#include "ui_desktop.h"
#include "ui_music_player.h"
#include "ui_navbar.h"
#include "ui_apps.h"
#include "ui_games.h"
#include "app_display.h"

static const char *TAG = "Main";

#define LCD_DRAW_BUFF_HEIGHT (160)   /* 绘制缓冲行数: 100→160, 减少整屏刷新时的分片flush次数, 提升动画流畅度 */
#define LVGL_REFRESH_RATE_MS (22)    /* 限制刷新率为约45Hz */

esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
static esp_lcd_panel_handle_t mipi_dpi_panel = NULL;
static esp_lcd_touch_handle_t tp = NULL;
static lv_display_t *lvgl_disp = NULL;

esp_lcd_panel_handle_t app_display_panel(void)
{
    return mipi_dpi_panel;
}

esp_lcd_touch_handle_t app_display_touch(void)
{
    return tp;
}

lv_display_t *app_display_lvgl_display(void)
{
    return lvgl_disp;
}

static const co6300_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0xF4, (uint8_t[]){0x5A}, 1, 0},
    {0xF5, (uint8_t[]){0x59}, 1, 0},
    {0xFE, (uint8_t[]){0x80}, 1, 0},
    {0xCD, (uint8_t[]){0x0F}, 1, 0},
    {0xCE, (uint8_t[]){0x05}, 1, 0},
    {0xCF, (uint8_t[]){0x05}, 1, 0},
    {0xD0, (uint8_t[]){0x05}, 1, 0},
    {0x66, (uint8_t[]){0x22}, 1, 0},
    {0x67, (uint8_t[]){0x44}, 1, 0},
    {0x68, (uint8_t[]){0x0A}, 1, 0},
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x71, (uint8_t[]){0x07}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x05}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x03, 0x9F}, 4, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 60},
};

static void lcd_vci_en_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << AMOLED_VCI_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(AMOLED_VCI_EN_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(AMOLED_VCI_EN_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void i2c_scan(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Found I2C device at address 0x%02x", addr);
        }
    }
    ESP_LOGI(TAG, "I2C scan completed.");
}

esp_err_t app_lcd_init(void)
{
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;

    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));

    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 1,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 360,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io));

    esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = MIPI_DSI_DPI_CLK_MHZ,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = MIPI_DSI_LCD_H_RES,
            .v_size = MIPI_DSI_LCD_V_RES,
            .hsync_back_porch = MIPI_DSI_LCD_HBP,
            .hsync_pulse_width = MIPI_DSI_LCD_HSYNC,
            .hsync_front_porch = MIPI_DSI_LCD_HFP,
            .vsync_back_porch = MIPI_DSI_LCD_VBP,
            .vsync_pulse_width = MIPI_DSI_LCD_VSYNC,
            .vsync_front_porch = MIPI_DSI_LCD_VFP,
        },
        .flags = {
            .use_dma2d = true,
        },
    };

    co6300_vendor_config_t vendor_config = {0};
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    vendor_config.flags.use_mipi_interface = 1;
    vendor_config.mipi_config.dsi_bus = mipi_dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;

    esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = TFT_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_co6300(mipi_dbi_io, &lcd_dev_config, &mipi_dpi_panel));
    esp_lcd_panel_reset(mipi_dpi_panel);
    esp_lcd_panel_init(mipi_dpi_panel);

    assert(mipi_dbi_io);
    assert(mipi_dpi_panel);

    return ESP_OK;
}

esp_err_t app_touch_init(void)
{
    ESP_LOGI(TAG, "Initialize I2C bus");

    const gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << TOUCH_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(TOUCH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TOUCH_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = TOUCH_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = TOUCH_I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_HOST, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_HOST, i2c_conf.mode, 0, 0, 0));
#if BSP_I2C_SCAN_AT_BOOT
    i2c_scan();
#endif

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST3530_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_I2C_HOST, &tp_io_config, &tp_io_handle));

    gpio_set_pull_mode(TOUCH_INT_GPIO, GPIO_PULLUP_ONLY);

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = MIPI_DSI_LCD_H_RES,
        .y_max = MIPI_DSI_LCD_V_RES,
        .rst_gpio_num = TOUCH_RST_GPIO,
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_LOGI(TAG, "Initialize touch controller");
    return esp_lcd_touch_new_i2c_cst3530(tp_io_handle, &tp_cfg, &tp);
}

esp_err_t app_lvgl_init(void)
{
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 4096 * 2,
        /* 双核分工 (调研结论: 音频/UI 互卡的标准解法):
         * LVGL 钉核 1; 核 0 留给 Bluedroid (BTC/SBC 编码, pinned core 0)
         * 与 ESP-Hosted SDIO。否则 LVGL 随机落到核 0, 会被高频 SBC 编码
         * 抢占, 表现为播放时拖动滑块/滑动卡顿。 */
        .task_affinity = 1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = LVGL_REFRESH_RATE_MS,  /* 限制刷新率约45Hz */
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port initialization failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = mipi_dbi_io,
        .panel_handle = mipi_dpi_panel,
        .buffer_size = MIPI_DSI_LCD_H_RES * LCD_DRAW_BUFF_HEIGHT,
        .double_buffer = true,
        .hres = MIPI_DSI_LCD_H_RES,
        .vres = MIPI_DSI_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .swap_bytes = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = {
            .avoid_tearing = false,
        },
    };
    lvgl_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_disp,
        .handle = tp,
    };
    lvgl_port_add_touch(&touch_cfg);

    return ESP_OK;
}

/* WiFi/BT 初始化策略:
 * 开机不做任何无线初始化 (SDIO 链路拉起 + Bluedroid/WiFi 协议栈都会
 * 消耗大量内存和时间)。所有无线功能由 bsp_wireless_mgr 统一管理:
 * UI 打开蓝牙/WiFi 页开关或发起扫描时, 才按需异步拉起对应协议栈,
 * 关闭时完整反初始化回收内存。内部 RAM 紧张时 bsp_memory 回收钩子
 * 会自动停掉"已开启但未连接"的空闲协议栈。 */

/* 后台音乐扫描完成 (运行在扫描任务上下文):
 * 把播放列表拷进 UI 的 PSRAM 名称缓冲 (不直接创建 LVGL 对象),
 * 锁内调用保证与 LVGL 线程互斥。 */
static void on_music_scan_done(int count)
{
    if (count <= 0) return;

    static char (*names)[128] = NULL;
    if (names == NULL) {
        names = (char (*)[128])bsp_memory_malloc(256 * 128);
    }
    if (names == NULL) return;

    int n = audio_player_fill_names(names, 256);
    const char *names_ptr[256];
    for (int i = 0; i < n; i++) names_ptr[i] = names[i];

    lvgl_port_lock(-1);
    ui_music_player_update_playlist(names_ptr, n,
                                    audio_player_get_current_index());
    lvgl_port_unlock();
    ESP_LOGI(TAG, "播放列表已推送到UI: %d 首", n);
}

void app_main(void)
{
    /* 0. NVS初始化(WiFi/BT需要) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 0.5 内存管理初始化 (PSRAM 监控) */
    bsp_memory_init();

    /* 1. LCD电源和初始化 */
    lcd_vci_en_init();
    ESP_ERROR_CHECK(app_lcd_init());
    ESP_ERROR_CHECK(app_touch_init());
    ESP_ERROR_CHECK(app_lvgl_init());

    /* 2. SD卡初始化 */
    ret = bsp_sd_card_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD卡初始化失败: %s", esp_err_to_name(ret));
    }

    /* 2.5 确保音乐目录存在 */
    if (bsp_sd_card_is_mounted()) {
        struct stat st;
        if (stat(MUSIC_DIR_PATH, &st) != 0) {
            if (mkdir(MUSIC_DIR_PATH, 0777) == 0) {
                ESP_LOGI(TAG, "创建音乐目录: %s", MUSIC_DIR_PATH);
            } else {
                ESP_LOGW(TAG, "无法创建音乐目录: %s", MUSIC_DIR_PATH);
            }
        }
    }

    /* 3. 亮度初始化(从SD卡恢复) */
    ret = bsp_brightness_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "亮度初始化失败: %s", esp_err_to_name(ret));
    }

    /* 3.5 主题初始化(从SD卡恢复) */
    bsp_theme_init();

    /* 4. 音频初始化(CS43131 + I2S) */
    ret = bsp_audio_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败: %s", esp_err_to_name(ret));
    }

    /* 5. 音频播放器初始化 */
    ret = audio_player_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "播放器初始化失败: %s", esp_err_to_name(ret));
    }

    /* 7. 预初始化音乐播放器页面 + 启动桌面UI (优先启动UI)
     *    必须先于UI启动, 这样LVGL线程才能拿到初始化的播放列表. */
    ESP_LOGI(TAG, "STEP: lvgl lock + music_player_init begin");
    lvgl_port_lock(-1);
    ui_music_player_init();
    ESP_LOGI(TAG, "STEP: music_player_init done");
    ESP_LOGI(TAG, "UI_INIT_BEGIN");
    ESP_LOGI(TAG, "UI_INIT_DESKTOP_BEGIN");
    ui_desktop_init();
    ESP_LOGI(TAG, "UI_INIT_DESKTOP_DONE");
    ESP_LOGI(TAG, "UI_INIT_APPS_BEGIN");
    ui_apps_init();
    ESP_LOGI(TAG, "UI_INIT_APPS_DONE");
    ui_games_init();
    ESP_LOGI(TAG, "UI_INIT_NAVBAR_BEGIN");
    ui_navbar_init();
    ESP_LOGI(TAG, "UI_INIT_NAVBAR_DONE");
    lvgl_port_unlock();
    ESP_LOGI(TAG, "UI_INIT_UNLOCK_DONE");

    /* 6. 音乐扫描放后台任务: 逐文件 ID3 解析耗时较长, 不再阻塞开机。
     *    扫描完成后经 on_music_scan_done 把播放列表推给 UI。 */
    audio_player_set_scan_done_cb(on_music_scan_done);
    if (bsp_sd_card_is_mounted()) {
        ret = audio_player_scan_music_async(MUSIC_DIR_PATH);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "后台扫描启动失败: %s", esp_err_to_name(ret));
        }
    }

    /* 8. 无线子系统: 仅初始化管理器 (worker 线程 + 内存回收钩子),
     *    BT/WiFi 协议栈由 UI 按需异步拉起, 开机零开销 */
    ret = bsp_wireless_mgr_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "无线管理器初始化失败: %s", esp_err_to_name(ret));
    }
    /* 管理器事件 -> LVGL 线程桥接 (UI 开关/状态实时刷新) */
    ui_wireless_bridge_init();

    ESP_LOGI(TAG, "系统初始化完成 (无线按需启动)");
}
