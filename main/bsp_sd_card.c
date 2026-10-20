/**
 * @file bsp_sd_card.c
 * @brief SD卡驱动模块实现（ESP32-P4 SDMMC Slot 0）
 *
 * 引脚定义（IO MUX固定引脚，见 bsp_board.h）：
 *   D0=SD_MMC_D0_PIN(39), D1=SD_MMC_D1_PIN(40), D2=SD_MMC_D2_PIN(41), D3=SD_MMC_D3_PIN(42)
 *   CLK=SD_MMC_CLK_PIN(43), CMD=SD_MMC_CMD_PIN(44)
 */
#include "bsp_sd_card.h"
#include <stdbool.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_default_configs.h"
#include "sd_pwr_ctrl.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "bsp_sd_card";

/* SDMMC IO 电源使用的片上LDO通道ID */
#define SDMMC_PWR_LDO_CHANNEL   4

static sdmmc_card_t *s_card = NULL;
static sd_pwr_ctrl_handle_t s_pwr_ctrl_handle = NULL;
static bool s_mounted = false;

esp_err_t bsp_sd_card_init(void)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "SD卡已挂载");
        return ESP_OK;
    }

    /* 配置片上LDO为SDMMC IO供电（通道4） */
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = SDMMC_PWR_LDO_CHANNEL,
    };
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建片上LDO失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* SDMMC主机配置，使用Slot 0（IO MUX固定引脚） */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.pwr_ctrl_handle = s_pwr_ctrl_handle;

    /* Slot配置：ESP32-P4默认引脚与bsp_board.h定义一致
     * SD_MMC_D0_PIN=39, SD_MMC_D1_PIN=40, SD_MMC_D2_PIN=41, SD_MMC_D3_PIN=42
     * SD_MMC_CLK_PIN=43, SD_MMC_CMD_PIN=44
     */
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;  /* 4位模式 */

    /* FAT文件系统挂载配置
     * max_files 放宽到 15: 系统多个模块会并发打开文件
     * (音乐播放/配置读写/游戏存档), 默认10个句柄可能被占满,
     * 导致 fopen 创建新配置文件失败 (亮度保存曾出现此问题). */
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 15,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "挂载SD卡失败: %s", esp_err_to_name(ret));
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl_handle);
        s_pwr_ctrl_handle = NULL;
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD卡挂载成功，挂载点: %s，总线宽度: 4", SD_MOUNT_POINT);
    return ESP_OK;
}

bool bsp_sd_card_is_mounted(void)
{
    return s_mounted;
}

esp_err_t bsp_sd_card_deinit(void)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "SD卡未挂载");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "卸载SD卡失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_card = NULL;
    s_mounted = false;

    /* 释放LDO电源控制资源 */
    if (s_pwr_ctrl_handle) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl_handle);
        s_pwr_ctrl_handle = NULL;
    }

    ESP_LOGI(TAG, "SD卡已卸载");
    return ESP_OK;
}
