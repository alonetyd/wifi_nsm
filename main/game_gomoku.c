#include "game_gomoku.h"
#include "ui_navbar.h"
#include "bsp_board.h"
#include "bsp_theme.h"
#include "esp_2_41ttf.h"
#include "esp_random.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GOMOKU_COLS 15
#define GOMOKU_ROWS 21
#define GOMOKU_CELLS (GOMOKU_COLS * GOMOKU_ROWS)
#define GOMOKU_CELL 16
#define GOMOKU_BOARD_W (GOMOKU_COLS * GOMOKU_CELL)
#define GOMOKU_BOARD_H (GOMOKU_ROWS * GOMOKU_CELL)

typedef enum { GOMOKU_EASY, GOMOKU_HARD, GOMOKU_EXPERT } gomoku_level_t;
static lv_obj_t *s_screen;
static lv_obj_t *s_board;
static lv_obj_t *s_board_label;
static lv_obj_t *s_status;
static uint8_t s_map[GOMOKU_CELLS];
static uint16_t s_history[GOMOKU_CELLS];
static int s_history_count;
static gomoku_level_t s_level = GOMOKU_EASY;
static bool s_computer = true;
static bool s_game_over;

static void set_status(const char *text)
{
    if (s_status) lv_label_set_text(s_status, text);
}

static void draw_board(void)
{
    char text[GOMOKU_ROWS * (GOMOKU_COLS + 1) + 1];
    int at = 0;
    for (int y = 0; y < GOMOKU_ROWS; y++) {
        for (int x = 0; x < GOMOKU_COLS; x++) {
            uint8_t v = s_map[y * GOMOKU_COLS + x];
            text[at++] = v == 1 ? 'X' : (v == 2 ? 'O' : '+');
        }
        text[at++] = '\n';
    }
    text[at] = '\0';
    lv_label_set_text(s_board_label, text);
}

static bool won(int p, uint8_t side)
{
    int x = p % GOMOKU_COLS;
    int y = p / GOMOKU_COLS;
    const int dirs[][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
    for (int d = 0; d < 4; d++) {
        int count = 1;
        for (int sign = -1; sign <= 1; sign += 2) {
            int nx = x + dirs[d][0] * sign;
            int ny = y + dirs[d][1] * sign;
            while (nx >= 0 && nx < GOMOKU_COLS && ny >= 0 && ny < GOMOKU_ROWS &&
                   s_map[ny * GOMOKU_COLS + nx] == side) {
                count++;
                nx += dirs[d][0] * sign;
                ny += dirs[d][1] * sign;
            }
        }
        if (count >= 5) return true;
    }
    return false;
}

static int choose_ai_move(void)
{
    int best = -1;
    int best_score = -1;
    for (int p = 0; p < GOMOKU_CELLS; p++) {
        if (s_map[p]) continue;
        int x = p % GOMOKU_COLS;
        int y = p / GOMOKU_COLS;
        int score = s_level == GOMOKU_EASY ? 1 : 0;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx;
                int ny = y + dy;
                if (nx >= 0 && nx < GOMOKU_COLS && ny >= 0 && ny < GOMOKU_ROWS &&
                    s_map[ny * GOMOKU_COLS + nx]) score += s_level == GOMOKU_EXPERT ? 5 : 2;
            }
        }
        s_map[p] = 2;
        if (s_level == GOMOKU_EXPERT && won(p, 2)) score += 1000;
        s_map[p] = 0;
        if (score > best_score || (score == best_score && (esp_random() & 1))) {
            best = p;
            best_score = score;
        }
    }
    return best;
}

static void reset_game(lv_event_t *e)
{
    (void)e;
    memset(s_map, 0, sizeof(s_map));
    s_history_count = 0;
    s_game_over = false;
    set_status(s_computer ? "人机" : "双人");
    draw_board();
}

static void undo_game(lv_event_t *e)
{
    (void)e;
    if (s_history_count == 0) return;
    s_map[s_history[--s_history_count]] = 0;
    if (s_computer && s_history_count > 0) s_map[s_history[--s_history_count]] = 0;
    s_game_over = false;
    set_status("已悔棋");
    draw_board();
}

static void play_at(int p)
{
    if (p < 0 || p >= GOMOKU_CELLS || s_map[p] || s_game_over) return;
    s_map[p] = 1;
    s_history[s_history_count++] = (uint16_t)p;
    draw_board();
    if (won(p, 1)) { s_game_over = true; set_status("黑方胜"); return; }
    if (!s_computer) { set_status("白方回合"); return; }
    int ai = choose_ai_move();
    if (ai >= 0) {
        s_map[ai] = 2;
        s_history[s_history_count++] = (uint16_t)ai;
        draw_board();
        if (won(ai, 2)) { s_game_over = true; set_status("白方胜"); }
    }
}

static void board_click(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_area_t area;
    lv_obj_get_coords(s_board, &area);
    int x = (point.x - area.x1) / GOMOKU_CELL;
    int y = (point.y - area.y1) / GOMOKU_CELL;
    if (x >= 0 && x < GOMOKU_COLS && y >= 0 && y < GOMOKU_ROWS) play_at(y * GOMOKU_COLS + x);
}

static void back_cb(lv_event_t *e) { (void)e; ui_navbar_back(); }
static void mode_cb(lv_event_t *e) { s_computer = (bool)(intptr_t)lv_event_get_user_data(e); reset_game(NULL); }
static void level_cb(lv_event_t *e) { s_level = (gomoku_level_t)(intptr_t)lv_event_get_user_data(e); reset_game(NULL); }

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *data)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, 58, 30);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, data);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &esp_2, 0);
    lv_obj_center(label);
    return button;
}

void game_gomoku_show(void)
{
    if (!s_screen) {
        s_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_screen, bsp_theme_colors()->bg, 0);
        lv_obj_set_style_pad_all(s_screen, 0, 0);

        lv_obj_t *back = small_button(s_screen, LV_SYMBOL_LEFT, back_cb, NULL);
        lv_obj_set_size(back, 44, 30);
        lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 8);

        lv_obj_t *title = lv_label_create(s_screen);
        lv_label_set_text(title, "五子棋");
        lv_obj_set_style_text_font(title, &esp_2, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

        s_status = lv_label_create(s_screen);
        lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -6, 10);

        s_board = lv_obj_create(s_screen);
        lv_obj_set_size(s_board, GOMOKU_BOARD_W, GOMOKU_BOARD_H);
        lv_obj_align(s_board, LV_ALIGN_TOP_MID, 0, 46);
        lv_obj_set_style_bg_color(s_board, lv_color_hex(0xc99b5a), 0);
        lv_obj_set_style_border_width(s_board, 1, 0);
        lv_obj_set_style_pad_all(s_board, 2, 0);
        lv_obj_clear_flag(s_board, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_board, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_board, board_click, LV_EVENT_CLICKED, NULL);

        s_board_label = lv_label_create(s_board);
        lv_obj_set_style_text_font(s_board_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_board_label, lv_color_hex(0x5a3515), 0);
        lv_obj_set_style_pad_all(s_board_label, 0, 0);
        lv_obj_align(s_board_label, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *controls = lv_obj_create(s_screen);
        lv_obj_set_size(controls, 250, 112);
        lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, -52);
        lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(controls, 0, 0);
        lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        small_button(controls, "人机", mode_cb, (void *)true);
        small_button(controls, "双人", mode_cb, (void *)false);
        small_button(controls, "简单", level_cb, (void *)GOMOKU_EASY);
        small_button(controls, "困难", level_cb, (void *)GOMOKU_HARD);
        small_button(controls, "复杂", level_cb, (void *)GOMOKU_EXPERT);
        small_button(controls, "悔棋", undo_game, NULL);
        small_button(controls, "重来", reset_game, NULL);
    }
    reset_game(NULL);
    ui_navbar_push_screen_named(s_screen, false, "五子棋");
}
