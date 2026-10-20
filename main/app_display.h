#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 LVGL 显示对象，避免头文件依赖整个 lvgl.h */
struct _lv_display_t;

esp_lcd_panel_handle_t app_display_panel(void);
esp_lcd_touch_handle_t app_display_touch(void);
struct _lv_display_t *app_display_lvgl_display(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DISPLAY_H */
