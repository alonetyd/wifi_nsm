#include "game_2048.h"
#include "ui_navbar.h"
#include "bsp_board.h"
#include "bsp_sd_card.h"
#include "bsp_theme.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "bsp_memory.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_W MIPI_DSI_LCD_H_RES
#define GAME_H MIPI_DSI_LCD_V_RES
#define GRID_SIZE 4
#define TILE_SIZE 52
#define TILE_SPACING 8
#define GRID_PAD 8
#define BOARD_SIZE (GRID_PAD * 2 + TILE_SIZE * GRID_SIZE + TILE_SPACING * (GRID_SIZE - 1))
#define BOARD_Y 64
#define INFO_Y (BOARD_Y + BOARD_SIZE + 12)
#define MOVE_COOLDOWN_MS 80
#define MOVE_ANIM_TIME 150

typedef enum { G2048_PLAYING, G2048_OVER } g2048_state_t;

typedef struct {
    int grid[GRID_SIZE][GRID_SIZE];
    int score;
    int best_score;
    g2048_state_t state;
    lv_obj_t *screen;
    lv_obj_t *board;
    lv_obj_t *score_label;
    lv_obj_t *best_label;
    lv_obj_t *score_card;
    lv_obj_t *best_card;
    lv_obj_t *game_over_label;
    lv_obj_t *tiles[GRID_SIZE][GRID_SIZE];
    lv_obj_t *numbers[GRID_SIZE][GRID_SIZE];
    int last_drawn_score;
    int last_drawn_best;
    uint32_t last_move_ms;
    bool animating;
    int spawn_r;
    int spawn_c;
    int active_animations;
    int remaining_animations;
    struct {
        lv_obj_t *tile;
        int start_x;
        int start_y;
        int target_x;
        int target_y;
    } animations[GRID_SIZE * GRID_SIZE];
} game_2048_data_t;

static game_2048_data_t *g2048;
static lv_point_t s_swipe_start;

static void draw_board(void);
static bool s_swipe_pressed;

static const uint32_t s_tile_colors[] = {
    0xcdc1b4, 0xeee4da, 0xede0c8, 0xf2b179, 0xf59563,
    0xf67c5f, 0xf65e3b, 0xedcf72, 0xedcc61, 0xedc850,
    0xedc53f, 0xedc22e
};

static uint32_t game_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int tile_color_index(int value)
{
    int index = 0;
    while (value > 1) {
        value >>= 1;
        index++;
    }
    return index > 11 ? 11 : index;
}

static const lv_font_t *tile_font(int value)
{
    if (value < 1000) return &lv_font_montserrat_24;
    if (value < 10000) return &lv_font_montserrat_20;
    return &lv_font_montserrat_16;
}

static bool save_game(void)
{
    if (!g2048 || !bsp_sd_card_is_mounted()) return false;
    FILE *f = fopen("/sdcard/game_2048.dat", "wb");
    if (!f) return false;
    bool ok = fwrite(g2048->grid, sizeof(g2048->grid), 1, f) == 1 &&
              fwrite(&g2048->score, sizeof(g2048->score), 1, f) == 1;
    fflush(f);
    fclose(f);
    return ok;
}

static bool load_game(void)
{
    if (!g2048 || !bsp_sd_card_is_mounted()) return false;
    FILE *f = fopen("/sdcard/game_2048.dat", "rb");
    if (!f) return false;
    bool ok = fread(g2048->grid, sizeof(g2048->grid), 1, f) == 1 &&
              fread(&g2048->score, sizeof(g2048->score), 1, f) == 1;
    fclose(f);
    if (!ok) return false;
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (g2048->grid[r][c] < 0 || g2048->grid[r][c] > 262144) return false;
        }
    }
    return true;
}

static int load_best_score(void)
{
    if (!bsp_sd_card_is_mounted()) return 0;
    FILE *f = fopen("/sdcard/game_2048_best.dat", "rb");
    if (!f) return 0;
    int score = 0;
    fread(&score, sizeof(score), 1, f);
    fclose(f);
    return score < 0 ? 0 : score;
}

static void save_best_score(void)
{
    if (!g2048 || !bsp_sd_card_is_mounted()) return;
    FILE *f = fopen("/sdcard/game_2048_best.dat", "wb");
    if (!f) return;
    fwrite(&g2048->best_score, sizeof(g2048->best_score), 1, f);
    fflush(f);
    fclose(f);
}

static void add_random_tile(void)
{
    if (!g2048) return;
    int empty[GRID_SIZE * GRID_SIZE][2];
    int count = 0;
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (g2048->grid[r][c] == 0) {
                empty[count][0] = r;
                empty[count][1] = c;
                count++;
            }
        }
    }
    if (count == 0) return;
    int index = (int)(esp_random() % (uint32_t)count);
    g2048->spawn_r = empty[index][0];
    g2048->spawn_c = empty[index][1];
    g2048->grid[g2048->spawn_r][g2048->spawn_c] =
        (esp_random() % 10U < 9U) ? 2 : 4;
}

static void init_game(bool new_game)
{
    if (!g2048) {
        g2048 = (game_2048_data_t *)bsp_memory_calloc(1, sizeof(game_2048_data_t));
    }
    if (!g2048) return;

    int best = load_best_score();
    memset(g2048->grid, 0, sizeof(g2048->grid));
    g2048->score = 0;
    g2048->best_score = best;
    g2048->state = G2048_PLAYING;
    g2048->last_drawn_score = -1;
    g2048->last_drawn_best = -1;
    g2048->last_move_ms = 0;
    g2048->animating = false;
    g2048->spawn_r = -1;
    g2048->spawn_c = -1;
    g2048->active_animations = 0;
    g2048->remaining_animations = 0;

    if (!new_game && load_game()) return;

    add_random_tile();
    add_random_tile();
}

static void animation_exec(void *var, int32_t value)
{
    lv_obj_t *tile = (lv_obj_t *)var;
    if (!g2048 || !tile) return;

    for (int index = 0; index < g2048->active_animations; index++) {
        if (g2048->animations[index].tile != tile) continue;
        const int x = g2048->animations[index].start_x +
                      (g2048->animations[index].target_x - g2048->animations[index].start_x) * value / 100;
        const int y = g2048->animations[index].start_y +
                      (g2048->animations[index].target_y - g2048->animations[index].start_y) * value / 100;
        lv_obj_set_pos(tile, x, y);
        return;
    }
}

static void animation_completed(lv_anim_t *anim)
{
    (void)anim;
    if (!g2048 || g2048->remaining_animations <= 0) return;
    g2048->remaining_animations--;
    if (g2048->remaining_animations == 0) {
        g2048->animating = false;
        draw_board();
    }
}

static void start_move_animation(int direction)
{
    if (!g2048 || !g2048->board) return;
    g2048->active_animations = 0;
    g2048->remaining_animations = 0;
    g2048->animating = true;

    int step_x = 0;
    int step_y = 0;
    if (direction < 0 || direction > 3) return;
    if (direction == 1) step_x = TILE_SIZE + TILE_SPACING;
    if (direction == 3) step_x = -(TILE_SIZE + TILE_SPACING);
    if (direction == 2) step_y = TILE_SIZE + TILE_SPACING;
    if (direction == 0) step_y = -(TILE_SIZE + TILE_SPACING);

    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (g2048->grid[r][c] == 0) continue;
            if (r == g2048->spawn_r && c == g2048->spawn_c) continue;
            lv_obj_t *tile = g2048->tiles[r][c];
            int target_x = GRID_PAD + c * (TILE_SIZE + TILE_SPACING);
            int target_y = GRID_PAD + r * (TILE_SIZE + TILE_SPACING);
            int start_x = target_x - step_x;
            int start_y = target_y - step_y;
            int index = g2048->active_animations++;
            g2048->animations[index].tile = tile;
            g2048->animations[index].start_x = start_x;
            g2048->animations[index].start_y = start_y;
            g2048->animations[index].target_x = target_x;
            g2048->animations[index].target_y = target_y;
            lv_obj_set_pos(tile, start_x, start_y);
        }
    }

    /* 在起点位置直接刷新最终数字/颜色: 滑动过程中就能看到正确的数字,
     * 避免"数字消失/闪删"的观感. (draw_board只改样式, 不改位置) */
    draw_board();

    g2048->remaining_animations = g2048->active_animations;
    for (int index = 0; index < g2048->active_animations; index++) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, g2048->animations[index].tile);
        lv_anim_set_exec_cb(&anim, animation_exec);
        lv_anim_set_values(&anim, 0, 100);
        lv_anim_set_duration(&anim, MOVE_ANIM_TIME);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_set_completed_cb(&anim, animation_completed);
        lv_anim_start(&anim);
    }
    if (g2048->active_animations == 0) {
        g2048->animating = false;
    }
}

static bool can_move(void)
{
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (g2048->grid[r][c] == 0) return true;
            if (c + 1 < GRID_SIZE && g2048->grid[r][c] == g2048->grid[r][c + 1]) return true;
            if (r + 1 < GRID_SIZE && g2048->grid[r][c] == g2048->grid[r + 1][c]) return true;
        }
    }
    return false;
}

static bool move_line(int line[GRID_SIZE], bool reverse)
{
    int source[GRID_SIZE] = {0};
    int result[GRID_SIZE] = {0};
    int count = 0;
    bool merged[GRID_SIZE] = {false};

    for (int i = 0; i < GRID_SIZE; i++) {
        int index = reverse ? GRID_SIZE - 1 - i : i;
        if (line[index] != 0) source[count++] = line[index];
    }

    int out = 0;
    for (int i = 0; i < count; i++) {
        if (out > 0 && result[out - 1] == source[i] && !merged[out - 1]) {
            result[out - 1] *= 2;
            merged[out - 1] = true;
            g2048->score += result[out - 1];
        } else {
            result[out++] = source[i];
        }
    }

    bool changed = false;
    for (int i = 0; i < GRID_SIZE; i++) {
        int index = reverse ? GRID_SIZE - 1 - i : i;
        if (line[index] != result[i]) changed = true;
        line[index] = result[i];
    }
    return changed;
}

static bool move_board(int direction)
{
    bool changed = false;
    if (direction == 3 || direction == 1) {
        for (int r = 0; r < GRID_SIZE; r++) {
            changed |= move_line(g2048->grid[r], direction == 1);
        }
    } else {
        int line[GRID_SIZE];
        for (int c = 0; c < GRID_SIZE; c++) {
            for (int r = 0; r < GRID_SIZE; r++) line[r] = g2048->grid[r][c];
            changed |= move_line(line, direction == 2);
            for (int r = 0; r < GRID_SIZE; r++) g2048->grid[r][c] = line[r];
        }
    }
    return changed;
}

static void set_tile_style(int r, int c, int value)
{
    lv_obj_t *tile = g2048->tiles[r][c];
    lv_obj_t *label = g2048->numbers[r][c];
    if (value == 0) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0xcdc1b4), LV_PART_MAIN);
        return;
    }

    int color_index = tile_color_index(value);
    lv_obj_set_style_bg_color(tile, lv_color_hex(s_tile_colors[color_index]), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    char text[12];
    snprintf(text, sizeof(text), "%d", value);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, tile_font(value), LV_PART_MAIN);
    lv_obj_set_style_text_color(label,
                                 color_index <= 1 ? lv_color_hex(0x776e65) : lv_color_white(),
                                 LV_PART_MAIN);
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
}

static void draw_board(void)
{
    if (!g2048 || !g2048->board) return;
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            set_tile_style(r, c, g2048->grid[r][c]);
        }
    }

    if (g2048->score != g2048->last_drawn_score) {
        char text[24];
        snprintf(text, sizeof(text), "%d", g2048->score);
        lv_label_set_text(g2048->score_label, text);
        g2048->last_drawn_score = g2048->score;
    }
    if (g2048->best_score != g2048->last_drawn_best) {
        char text[24];
        snprintf(text, sizeof(text), "%d", g2048->best_score);
        lv_label_set_text(g2048->best_label, text);
        g2048->last_drawn_best = g2048->best_score;
    }
}

static void perform_move(int direction)
{
    if (!g2048 || g2048->state != G2048_PLAYING || g2048->animating) return;
    uint32_t now = game_millis();
    if (now - g2048->last_move_ms < MOVE_COOLDOWN_MS) return;
    if (!move_board(direction)) return;

    g2048->last_move_ms = now;
    add_random_tile();
    if (g2048->score > g2048->best_score) {
        g2048->best_score = g2048->score;
        save_best_score();
    }
    save_game();
    /* 样式刷新在 start_move_animation 内完成: 先快照到起点, 再刷最终样式,
     * 随后动画滑入目标位置. 这样滑动过程中就能看到正确的数字,
     * 不会出现"数字先闪到最终位置再跳回"或"滑动时数字消失"的现象. */
    start_move_animation(direction);
    if (!can_move()) {
        g2048->state = G2048_OVER;
        lv_obj_clear_flag(g2048->game_over_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void board_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev) {
            lv_indev_get_point(indev, &s_swipe_start);
            s_swipe_pressed = true;
        }
        return;
    }
    if (code != LV_EVENT_RELEASED || !s_swipe_pressed) return;
    s_swipe_pressed = false;

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t end;
    lv_indev_get_point(indev, &end);
    int dx = end.x - s_swipe_start.x;
    int dy = end.y - s_swipe_start.y;
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    if (ax < 30 && ay < 30) return;
    if (ax > ay) perform_move(dx > 0 ? 1 : 3);
    else perform_move(dy > 0 ? 2 : 0);
}

static void restart_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !g2048) return;
    if (bsp_sd_card_is_mounted()) remove("/sdcard/game_2048.dat");
    memset(g2048->grid, 0, sizeof(g2048->grid));
    g2048->score = 0;
    g2048->state = G2048_PLAYING;
    g2048->last_move_ms = 0;
    g2048->last_drawn_score = -1;
    g2048->last_drawn_best = -1;
    lv_obj_add_flag(g2048->game_over_label, LV_OBJ_FLAG_HIDDEN);
    add_random_tile();
    add_random_tile();
    draw_board();
}

static void board_cleanup_cb(lv_event_t *e)
{
    (void)e;
    if (!g2048) return;
    g2048->animating = false;
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (g2048->tiles[r][c]) lv_anim_delete(g2048->tiles[r][c], animation_exec);
        }
    }
    g2048->board = NULL;
}

static void cleanup_cb(lv_event_t *e)
{
    (void)e;
    if (g2048) {
        g2048->screen = NULL;
        free(g2048);
        g2048 = NULL;
    }
}

static lv_obj_t *create_score_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                   const char *title, lv_obj_t **value_label)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 78, 36);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(card, bsp_theme_colors()->card, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(0x9ca3af), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 2);

    *value_label = lv_label_create(card);
    lv_label_set_text(*value_label, "0");
    lv_obj_set_style_text_color(*value_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(*value_label, LV_ALIGN_BOTTOM_MID, 0, -1);
    return card;
}

extern "C" void game_2048_show(void)
{
    init_game(false);
    if (!g2048) return;

    lv_obj_t *screen = lv_obj_create(NULL);
    g2048->screen = screen;
    lv_obj_set_style_bg_color(screen, bsp_theme_colors()->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, cleanup_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *nav = lv_obj_create(screen);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, GAME_W, UI_NAVBAR_H);
    lv_obj_align(nav, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(nav, bsp_theme_colors()->card, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(nav);
    lv_obj_set_size(back, 42, 34);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_event_cb(back, [](lv_event_t *) { ui_navbar_back(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);

    lv_obj_t *nav_title = lv_label_create(nav);
    lv_label_set_text(nav_title, "2048");
    lv_obj_set_style_text_color(nav_title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(nav_title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *restart = lv_btn_create(screen);
    lv_obj_set_size(restart, 34, 34);
    lv_obj_align(restart, LV_ALIGN_TOP_RIGHT, -6, INFO_Y);
    lv_obj_add_event_cb(restart, restart_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *restart_label = lv_label_create(restart);
    lv_label_set_text(restart_label, LV_SYMBOL_REFRESH);
    lv_obj_center(restart_label);

    g2048->score_card = create_score_card(screen, 8, INFO_Y, "分数", &g2048->score_label);
    g2048->best_card = create_score_card(screen, 94, INFO_Y, "最高", &g2048->best_label);

    g2048->board = lv_obj_create(screen);
    lv_obj_remove_style_all(g2048->board);
    lv_obj_set_size(g2048->board, BOARD_SIZE, BOARD_SIZE);
    lv_obj_align(g2048->board, LV_ALIGN_TOP_MID, 0, BOARD_Y);
    lv_obj_set_style_bg_color(g2048->board, lv_color_hex(0xbbada0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g2048->board, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g2048->board, 8, LV_PART_MAIN);
    lv_obj_clear_flag(g2048->board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g2048->board, board_cleanup_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_flag(g2048->board, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g2048->board, board_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g2048->board, board_event_cb, LV_EVENT_RELEASED, NULL);

    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            lv_obj_t *tile = lv_obj_create(g2048->board);
            lv_obj_remove_style_all(tile);
            lv_obj_set_size(tile, TILE_SIZE, TILE_SIZE);
            lv_obj_set_pos(tile, GRID_PAD + c * (TILE_SIZE + TILE_SPACING),
                           GRID_PAD + r * (TILE_SIZE + TILE_SPACING));
            lv_obj_set_style_bg_color(tile, lv_color_hex(0xcdc1b4), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(tile, 8, LV_PART_MAIN);
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *number = lv_label_create(tile);
            /* 不设置固定尺寸: 让label自动收缩到文字大小,
             * lv_obj_center()才能把数字真正居中到棋子正中央.
             * 若给label固定TILE_SIZE高度, 文字会贴着label顶部绘制, 数字会偏上. */
            lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_text_color(number, lv_color_hex(0x776e65), LV_PART_MAIN);
            lv_obj_set_style_text_font(number, &lv_font_montserrat_24, LV_PART_MAIN);
            lv_obj_center(number);
            lv_obj_add_flag(number, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(number, LV_OBJ_FLAG_CLICKABLE);
            g2048->tiles[r][c] = tile;
            g2048->numbers[r][c] = number;
        }
    }

    g2048->game_over_label = lv_label_create(screen);
    lv_label_set_text(g2048->game_over_label, "游戏结束\n点击右上角重新开始");
    lv_obj_set_style_text_align(g2048->game_over_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(g2048->game_over_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g2048->game_over_label, bsp_theme_colors()->card, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g2048->game_over_label, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g2048->game_over_label, 12, LV_PART_MAIN);
    lv_obj_align(g2048->game_over_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g2048->game_over_label, LV_OBJ_FLAG_HIDDEN);

    draw_board();
    ui_navbar_push_screen_named(screen, false, "2048");
}
