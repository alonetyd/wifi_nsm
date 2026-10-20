/*******************************************************************************
 * Chinese Font for ESP32 S3 2.41" AMOLED Display
 * 中文字体 - 20px
 * 
 * Size: 20px
 * Bpp: 1 bit-per-pixel
 * Range: 0x4E00-0x9FFF (CJK Unified Ideographs)
 * Fallback: lv_font_cn_20
 ******************************************************************************/

#ifndef ESP_2_41TTF_H
#define ESP_2_41TTF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// Main Chinese font (20px, CJK range 0x4E00-0x9FFF)
// Fallback: lv_font_montserrat_14 (for ASCII, numbers, symbols)
extern const lv_font_t esp_2;

#ifdef __cplusplus
}
#endif

#endif // ESP_2_41TTF_H

