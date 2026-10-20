/**
 * @file bsp_brightness.h
 * @brief LCD亮度控制模块
 *
 * 通过MIPI DBI接口发送LCD命令0x51设置背光亮度（0-255）。
 * 亮度值持久化保存到 NVS flash（主存储，断电不丢失），
 * 同时尽力写入SD卡文件 /sdcard/brightness.cfg（兼容旧版本）。
 */
#ifndef BSP_BRIGHTNESS_H
#define BSP_BRIGHTNESS_H

#include <stdint.h>
#include "esp_err.h"
#include "bsp_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化亮度模块
 *
 * 优先从NVS读取亮度配置，其次从SD卡文件读取，并应用到LCD。
 * 若均无记录，使用默认亮度值 BRIGHTNESS_DEFAULT。
 *
 * @return
 *      - ESP_OK 成功
 *      - 其他错误码表示失败
 */
esp_err_t bsp_brightness_init(void);

/**
 * @brief 设置亮度并保存
 *
 * 将亮度值限制在 [BRIGHTNESS_MIN, BRIGHTNESS_MAX] 范围内，
 * 通过LCD命令0x51设置背光，并防抖持久化到NVS + SD卡。
 *
 * @param level 亮度值（0-255，会自动限幅）
 * @return
 *      - ESP_OK 成功
 *      - ESP_ERR_INVALID_STATE MIPI DBI IO未初始化
 *      - 其他错误码表示失败
 */
esp_err_t bsp_brightness_set(uint8_t level);

/**
 * @brief 获取当前亮度值
 *
 * @return 当前亮度值（0-255）
 */
uint8_t bsp_brightness_get(void);

/**
 * @brief 直接设置亮度寄存器 (不限制最小值, 不持久化)
 *
 * 用于睡眠定时熄屏 (0) / 唤醒恢复等临时场景.
 * @param level 亮度值 0-255 (0 = 熄屏)
 */
esp_err_t bsp_brightness_set_raw(uint8_t level);

/**
 * @brief 立即保存当前亮度到NVS + SD卡
 *
 * @return
 *      - ESP_OK 成功 (NVS已保存, SD卡为尽力写入)
 *      - 其他错误码表示NVS保存失败
 */
esp_err_t bsp_brightness_save(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_BRIGHTNESS_H */
