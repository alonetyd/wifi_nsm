#include "game_snake.h"
#include "ui_navbar.h"
#include "bsp_board.h"
#include "bsp_theme.h"
#include "esp_2_41ttf.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "bsp_memory.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SNAKE_COLS 12
#define SNAKE_ROWS 24
#define SNAKE_MAX (SNAKE_COLS * SNAKE_ROWS)

typedef struct { int16_t x; int16_t y; } snake_node_t;
typedef enum { S_UP, S_RIGHT, S_DOWN, S_LEFT } snake_dir_t;

static lv_obj_t *s_screen;
static lv_obj_t *s_board_label;
static lv_obj_t *s_score_label;
static lv_timer_t *s_timer;
static snake_node_t *s_body;
static int s_length;
static int s_food;
static int s_score;
static snake_dir_t s_dir;
static snake_dir_t s_next_dir;

static void snake_back(lv_event_t *e) { (void)e; if (s_timer) lv_timer_pause(s_timer); ui_navbar_back(); }

static bool occupied(int x, int y)
{
    for (int i = 0; i < s_length; i++) {
        if (s_body[i].x == x && s_body[i].y == y) return true;
    }
    return false;
}

static void place_food(void)
{
    for (int n = 0; n < SNAKE_MAX; n++) {
        int p = (int)(esp_random() % SNAKE_MAX);
        if (!occupied(p % SNAKE_COLS, p / SNAKE_COLS)) { s_food = p; return; }
    }
}

static void draw_snake(void)
{
    char *text = (char *)bsp_memory_malloc((SNAKE_COLS + 1) * SNAKE_ROWS + 32);
    if (!text) return;
    int at = 0;
    for (int y = 0; y < SNAKE_ROWS; y++) {
        for (int x = 0; x < SNAKE_COLS; x++) {
            char c = '.';
            if (y * SNAKE_COLS + x == s_food) c = '*';
            for (int i = 0; i < s_length; i++) {
                if (s_body[i].x == x && s_body[i].y == y) { c = i == 0 ? 'O' : '#'; break; }
            }
            text[at++] = c;
        }
        text[at++] = '\n';
    }
    text[at] = '\0';
    lv_label_set_text(s_board_label, text);
    char score[24];
    snprintf(score, sizeof(score), "得分 %d", s_score);
    lv_label_set_text(s_score_label, score);
    free(text);
}

static void reset_snake(void)
{
    s_length = 4; s_score = 0; s_dir = S_RIGHT; s_next_dir = S_RIGHT;
    for (int i = 0; i < s_length; i++) { s_body[i].x = 5 - i; s_body[i].y = 10; }
    s_food = 8 * SNAKE_COLS + 8;
    draw_snake();
}

static void snake_tick(lv_timer_t *timer)
{
    (void)timer;
    s_dir = s_next_dir;
    snake_node_t head = s_body[0];
    if (s_dir == S_UP) head.y--; else if (s_dir == S_RIGHT) head.x++;
    else if (s_dir == S_DOWN) head.y++; else head.x--;
    if (head.x < 0 || head.x >= SNAKE_COLS || head.y < 0 || head.y >= SNAKE_ROWS || occupied(head.x, head.y)) {
        reset_snake(); return;
    }
    bool grow = head.y * SNAKE_COLS + head.x == s_food;
    if (grow && s_length < SNAKE_MAX) { s_length++; s_score += 10; place_food(); }
    for (int i = s_length - 1; i > 0; i--) s_body[i] = s_body[i - 1];
    s_body[0] = head;
    draw_snake();
}

static void snake_gesture(lv_event_t *e)
{
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (d == LV_DIR_TOP && s_dir != S_DOWN) s_next_dir = S_UP;
    else if (d == LV_DIR_RIGHT && s_dir != S_LEFT) s_next_dir = S_RIGHT;
    else if (d == LV_DIR_BOTTOM && s_dir != S_UP) s_next_dir = S_DOWN;
    else if (d == LV_DIR_LEFT && s_dir != S_RIGHT) s_next_dir = S_LEFT;
}

void game_snake_show(void)
{
    if (!s_screen) {
        s_body = (snake_node_t *)bsp_memory_calloc(SNAKE_MAX, sizeof(*s_body));
        s_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_screen, bsp_theme_colors()->bg, 0);
        lv_obj_set_style_pad_all(s_screen, 0, 0);
        lv_obj_add_event_cb(s_screen, snake_gesture, LV_EVENT_GESTURE, NULL);
        lv_obj_t *back = lv_btn_create(s_screen); lv_obj_set_size(back, 48, 38);
        lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 12); lv_obj_add_event_cb(back, snake_back, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT); lv_obj_set_style_text_font(bl, &esp_2, 0); lv_obj_center(bl);
        s_score_label = lv_label_create(s_screen); lv_obj_align(s_score_label, LV_ALIGN_TOP_RIGHT, -10, 20);
        s_board_label = lv_label_create(s_screen); lv_obj_align(s_board_label, LV_ALIGN_TOP_MID, 0, 90);
        lv_obj_set_style_text_font(s_board_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_board_label, bsp_theme_colors()->accent, 0);
    }
    reset_snake();
    if (s_timer) lv_timer_del(s_timer);
    s_timer = lv_timer_create(snake_tick, 220, NULL);
    ui_navbar_push_screen_named(s_screen, false, "贪吃蛇");
}
