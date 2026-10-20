#ifndef UI_MEDIA_APPS_H
#define UI_MEDIA_APPS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_gallery_show(void);
void ui_gallery_show_file(const char *path);
void ui_video_player_show(void);
void ui_video_player_show_file(const char *path);
void ui_sd_speed_test_show(void);

#ifdef __cplusplus
}
#endif

#endif
