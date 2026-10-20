/**
 * @file bsp_hw_test.h
 * @brief 硬件通信测试模块
 *
 * 测试外设芯片通信状态: I2C总线、音频DAC(CS43131)、SD卡、ESP-Hosted从机。
 * 不测试触摸和屏幕(这些在开机时已验证可用)。
 */
#ifndef BSP_HW_TEST_H
#define BSP_HW_TEST_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 测试项目枚举 */
typedef enum {
    HW_TEST_I2C_BUS = 0,    /* I2C总线 */
    HW_TEST_CS43131,        /* 音频DAC CS43131 */
    HW_TEST_SD_CARD,        /* SD卡 */
    HW_TEST_ESP_HOSTED,     /* ESP-Hosted SDIO从机 */
    HW_TEST_COUNT
} hw_test_id_t;

/* 单项测试结果 */
typedef struct {
    bool passed;            /* 是否通过 */
    char detail[80];        /* 详细信息 */
} hw_test_result_t;

/**
 * @brief 运行所有硬件测试
 * @param results 结果数组, 大小为 HW_TEST_COUNT
 */
void bsp_hw_test_run_all(hw_test_result_t *results);

/**
 * @brief 获取测试项名称
 */
const char *bsp_hw_test_name(hw_test_id_t id);

/**
 * @brief 获取测试项图标
 */
const char *bsp_hw_test_icon(hw_test_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* BSP_HW_TEST_H */
