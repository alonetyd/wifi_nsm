/**
 * @file bsp_sd_card.h
 * @brief SD卡驱动模块（ESP32-P4 SDMMC Slot 0）
 *
 * 使用ESP-IDF SDMMC API，通过Slot 0（IO MUX固定引脚）连接SD卡，
 * 支持4位模式和FAT文件系统，挂载点为 /sdcard。
 */
#ifndef BSP_SD_CARD_H
#define BSP_SD_CARD_H

#include "esp_err.h"
#include "bsp_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化SD卡并挂载FAT文件系统
 *
 * 配置片上LDO电源（通道4），初始化SDMMC主机使用Slot 0，
 * 以4位模式挂载SD卡到 /sdcard 挂载点。
 *
 * @return
 *      - ESP_OK 成功
 *      - 其他错误码表示失败
 */
esp_err_t bsp_sd_card_init(void);

/**
 * @brief 检查SD卡是否已挂载
 *
 * @return true 已挂载，false 未挂载
 */
bool bsp_sd_card_is_mounted(void);

/**
 * @brief 卸载SD卡
 *
 * 卸载FAT文件系统并释放SDMMC及LDO相关资源。
 *
 * @return
 *      - ESP_OK 成功
 *      - ESP_ERR_INVALID_STATE SD卡未挂载
 */
esp_err_t bsp_sd_card_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_SD_CARD_H */
