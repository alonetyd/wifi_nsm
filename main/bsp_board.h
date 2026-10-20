#ifndef BSP_BOARD_H
#define BSP_BOARD_H

#include "driver/gpio.h"

/* ===================== Debug 开关 ===================== */
/* 开机全总线 I2C 扫描 (127 地址探测, 硬件调试用, 量产置 0) */
#define BSP_I2C_SCAN_AT_BOOT    0

/* ===================== LCD (MIPI DSI CO6300) ===================== */
#define TFT_RST_GPIO            GPIO_NUM_23
#define AMOLED_VCI_EN_GPIO      GPIO_NUM_53

#define MIPI_DSI_DPI_CLK_MHZ    14  /* 降低时钟以限制刷新率为约45Hz */
#define MIPI_DSI_LCD_H_RES      262
#define MIPI_DSI_LCD_V_RES      928
#define MIPI_DSI_LCD_HSYNC      4
#define MIPI_DSI_LCD_HBP        32
#define MIPI_DSI_LCD_HFP        32
#define MIPI_DSI_LCD_VSYNC      4
#define MIPI_DSI_LCD_VBP        8
#define MIPI_DSI_LCD_VFP        8

#define MIPI_DSI_PHY_LDO_CHAN   3
#define MIPI_DSI_PHY_LDO_MV     2500

/* ===================== Touch (CST3530 I2C) ===================== */
#define TOUCH_I2C_SDA           GPIO_NUM_18
#define TOUCH_I2C_SCL           GPIO_NUM_19
#define TOUCH_INT_GPIO          GPIO_NUM_22
#define TOUCH_RST_GPIO          GPIO_NUM_21
#define TOUCH_I2C_HOST          I2C_NUM_0
#define TOUCH_I2C_FREQ_HZ       400000

/* ===================== SD Card (SDMMC Slot 0) ===================== */
#define SD_MMC_D0_PIN           39
#define SD_MMC_D1_PIN           40
#define SD_MMC_D2_PIN           41
#define SD_MMC_D3_PIN           42
#define SD_MMC_CLK_PIN          43
#define SD_MMC_CMD_PIN          44
#define SD_MOUNT_POINT          "/sdcard"

/* ===================== Audio I2S (CS43131) ===================== */
#define AUDIO_I2S_MCK_IO        (-1)
#define AUDIO_I2S_BCK_IO        16
#define AUDIO_I2S_WS_IO         17
#define AUDIO_I2S_DO_IO         54
#define AUDIO_I2S_DI_IO         (-1)

/* CS43131 DAC reset pin */
#define CS43131_RST_GPIO        GPIO_NUM_20

/* CS43131 I2C config (shares bus with touch) */
#define CS43131_I2C_HOST        I2C_NUM_0
#define CS43131_I2C_ADDR        0x30   /* 7-bit I2C address, actual scan result */

/* ===================== ESP-Hosted SDIO (Slot 1) ===================== */
#define ESP_HOSTED_SDIO_CMD     28
#define ESP_HOSTED_SDIO_CLK     31
#define ESP_HOSTED_SDIO_D0      27
#define ESP_HOSTED_SDIO_D1      26
#define ESP_HOSTED_SDIO_D2      30
#define ESP_HOSTED_SDIO_D3      29
#define ESP_HOSTED_SDIO_RST     32

/* ===================== MIC PDM ===================== */
#define MIC_PDM_DATA_PIN        4
#define MIC_PDM_SCK_PIN         3

/* ===================== Audio Player ===================== */
#define AUDIO_SAMPLE_RATE       44100
#define AUDIO_BITS_PER_SAMPLE   16
#define AUDIO_CHANNELS          2
#define AUDIO_I2S_PORT          I2S_NUM_0
#define AUDIO_CHUNK_SIZE        4096

/* ===================== LCD Brightness ===================== */
#define BRIGHTNESS_FILE         "/sdcard/brightness.cfg"
#define BRIGHTNESS_DEFAULT      200   /* 0-255 */
#define BRIGHTNESS_MIN          10
#define BRIGHTNESS_MAX          255

/* ===================== Volume ===================== */
#define VOLUME_FILE             "/sdcard/volume.cfg"
#define VOLUME_DEFAULT          70    /* 0-100 */

/* ===================== Music Directory ===================== */
#define MUSIC_DIR_PATH          "/sdcard/music"

#endif /* BSP_BOARD_H */
