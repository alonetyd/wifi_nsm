#ifndef BSP_AUDIO_H
#define BSP_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_audio_init(void);
esp_err_t bsp_audio_set_volume(uint8_t vol);  // 0-100
uint8_t bsp_audio_get_volume(void);
esp_err_t bsp_audio_write(const uint8_t *data, size_t len, size_t *bytes_written);
esp_err_t bsp_audio_set_sample_rate(uint32_t rate);

#ifdef __cplusplus
}
#endif

#endif /* BSP_AUDIO_H */
