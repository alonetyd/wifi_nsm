#include "ui_media_apps.h"
#include "bsp_board.h"
#include "bsp_sd_card.h"
#include "bsp_theme.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "ui_gesture.h"
#include "ui_navbar.h"
#include "esp_2_41ttf.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MEDIA_W MIPI_DSI_LCD_H_RES
#define MEDIA_H MIPI_DSI_LCD_V_RES
#define TEST_BUF_SIZE (64 * 1024)

static lv_obj_t *s_gallery;
static lv_obj_t *s_video;
static lv_obj_t *s_speed;
static lv_obj_t *s_gallery_list;
static lv_obj_t *s_video_label;
static lv_obj_t *s_speed_label;
static TaskHandle_t s_speed_task;
static volatile bool s_speed_running;
static char s_selected_path[256];

static lv_color_t media_bg(void) { return bsp_theme_colors()->bg; }
static lv_color_t media_card(void) { return bsp_theme_colors()->card; }
static lv_color_t media_accent(void) { return bsp_theme_colors()->accent; }

static lv_obj_t *media_label(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &esp_2, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    return label;
}

static void media_back(lv_event_t *e)
{
    (void)e;
    ui_navbar_back();
}

static lv_obj_t *media_screen(const char *title, lv_obj_t **title_label)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, media_bg(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *back = lv_btn_create(screen);
    lv_obj_set_size(back, 46, 42);
    lv_obj_set_pos(back, 4, 8);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, media_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &esp_2, 0);
    lv_obj_center(back_label);
    *title_label = media_label(screen, title, 56, 12, MEDIA_W - 64, 40);
    return screen;
}

static bool image_ext(const char *name)
{
    const char *ext = strrchr(name, '.');
    return ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") ||
                   !strcasecmp(ext, ".png") || !strcasecmp(ext, ".bmp"));
}

static bool video_ext(const char *name)
{
    const char *ext = strrchr(name, '.');
    return ext && (!strcasecmp(ext, ".avi") || !strcasecmp(ext, ".mjpeg"));
}

static void gallery_refresh(void)
{
    if (!s_gallery_list) return;
    lv_obj_clean(s_gallery_list);
    if (!bsp_sd_card_is_mounted()) {
        media_label(s_gallery_list, "SD卡未挂载", 8, 10, MEDIA_W - 24, 40);
        return;
    }
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        media_label(s_gallery_list, "无法打开 /sdcard", 8, 10, MEDIA_W - 24, 40);
        return;
    }
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_DIR || !image_ext(ent->d_name)) continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, ent->d_name);
        lv_obj_t *btn = lv_list_add_button(s_gallery_list, LV_SYMBOL_IMAGE, ent->d_name);
        lv_obj_add_event_cb(btn, (lv_event_cb_t)ui_gallery_show_file, LV_EVENT_CLICKED, strdup(path));
        count++;
    }
    closedir(dir);
    if (!count) lv_list_add_text(s_gallery_list, "未找到 jpg/jpeg/png/bmp 文件");
}

static void gallery_file_cb(lv_event_t *e)
{
    const char *path = lv_event_get_user_data(e);
    if (path) ui_gallery_show_file(path);
}

void ui_gallery_show(void)
{
    if (!s_gallery) {
        lv_obj_t *title;
        s_gallery = media_screen("图库", &title);
        (void)title;
        s_gallery_list = lv_list_create(s_gallery);
        lv_obj_set_pos(s_gallery_list, 8, 64);
        lv_obj_set_size(s_gallery_list, MEDIA_W - 16, MEDIA_H - 116);
        lv_obj_set_style_bg_color(s_gallery_list, media_bg(), 0);
        lv_obj_set_style_border_width(s_gallery_list, 0, 0);
        ui_edge_swipe_add(s_gallery, media_back);
    }
    gallery_refresh();
    ui_navbar_push_screen_named(s_gallery, true