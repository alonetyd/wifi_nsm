#include "gomoku_game.h"
#include "ui_navbar.h"
#include "game_canvas_compat.h"
#include "bsp_board.h"
#include "bsp_theme.h"
#include "esp_2_41ttf.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#define GAME_W MIPI_DSI_LCD_H_RES
#define GAME_H MIPI_DSI_LCD_V_RES
#define CN_FONT (&esp_2)
#include <math.h>
#include <stdlib.h>

// ==================== 五子棋游戏 ====================
// 15x21 棋盘，黑棋先手，五子连珠获胜
// AI: minimax + α-β 剪枝
// 三档难度：简单(深度1+随机)、困难(深度2+排序)、超级复杂(深度4+排序)

#define BOARD_W     15
#define BOARD_H     21
#define CANVAS_W    262
#define CANVAS_H    368
#define CELL_SIZE   17
#define MARGIN_X    12
#define MARGIN_Y    14
#define STONE_SIZE  14
#define REFRESH_MS  50

// 颜色
#define GK_BG        lv_color_hex(0x0A0A0F)
#define GK_BOARD     lv_color_hex(0xD4A76A)
#define GK_LINE      lv_color_hex(0x3D2B1F)
#define GK_BLACK     lv_color_hex(0x1A1A1A)
#define GK_WHITE     lv_color_hex(0xF5F5F5)
#define GK_LAST      lv_color_hex(0xE53E3E)
#define GK_STAR      lv_color_hex(0x2D1810)
#define GK_TEXT      lv_color_hex(0xE5E7EB)
#define GK_DIM       lv_color_hex(0x6B7280)
#define GK_INFO      lv_color_hex(0x3B82F6)
#define GK_SUCCESS   lv_color_hex(0x10B981)
#define GK_WARN      lv_color_hex(0xF59E0B)
#define GK_DANGER    lv_color_hex(0xDC2626)

// 状态
typedef enum { GK_MENU, GK_PLAYING, GK_GAMEOVER } gk_state_t;
typedef enum { GK_EASY, GK_HARD, GK_SUPER } gk_diff_t;
typedef enum { GK_PVE, GK_PVP } gk_mode_t;

// 全局状态
static gk_state_t s_state = GK_MENU;
static gk_diff_t s_diff = GK_HARD;
static gk_mode_t s_mode = GK_PVE;
static int s_board[BOARD_W][BOARD_H];
static int s_player = 1;          // 1=黑, 2=白
static int s_winner = 0;
static int s_last_x = -1, s_last_y = -1;
static int s_pending_x = -1, s_pending_y = -1;  // 待确认落子位置 (-1 = 无)
static int s_move_count = 0;
static bool s_ai_pending = false;

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_canvas = NULL;
static lv_color_t *s_buf = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_menu_panel = NULL;
static lv_obj_t *s_game_panel = NULL;
static lv_timer_t *s_timer = NULL;

// AI 临时数组（按深度分桶，避免递归覆盖；静态避免栈溢出）
// ~6.3KB × 2, 放 PSRAM BSS 省 internal RAM (AI 走 PSRAM 缓存, 延迟可接受)
#define MAX_SEARCH_DEPTH 5
static int s_cand_x[MAX_SEARCH_DEPTH][BOARD_W * BOARD_H] EXT_RAM_BSS_ATTR;
static int s_cand_y[MAX_SEARCH_DEPTH][BOARD_W * BOARD_H] EXT_RAM_BSS_ATTR;
static int s_cand_n[MAX_SEARCH_DEPTH];

// 历史记录（用于悔棋）
static int s_hist_x[BOARD_W * BOARD_H + 1];
static int s_hist_y[BOARD_W * BOARD_H + 1];
static int s_hist_n = 0;

// 菜单按钮引用
static lv_obj_t *s_diff_btns[3] = {NULL};
static lv_obj_t *s_mode_btns[2] = {NULL};

// ==================== 棋盘逻辑 ====================

static void board_clear(void) {
    memset(s_board, 0, sizeof(s_board));
    s_player = 1;
    s_winner = 0;
    s_last_x = -1; s_last_y = -1;
    s_pending_x = -1; s_pending_y = -1;
    s_move_count = 0;
    s_ai_pending = false;
    s_hist_n = 0;
}

static int check_win(int x, int y, int role) {
    static const int dx[4] = {0, 1, 1, 1};
    static const int dy[4] = {1, 0, 1, -1};
    for (int d = 0; d < 4; d++) {
        int cnt = 1;
        for (int s = 1; s < 5; s++) {
            int nx = x + dx[d]*s, ny = y + dy[d]*s;
            if (nx<0||nx>=BOARD_W||ny<0||ny>=BOARD_H) break;
            if (s_board[nx][ny] != role) break;
            cnt++;
        }
        for (int s = 1; s < 5; s++) {
            int nx = x - dx[d]*s, ny = y - dy[d]*s;
            if (nx<0||nx>=BOARD_W||ny<0||ny>=BOARD_H) break;
            if (s_board[nx][ny] != role) break;
            cnt++;
        }
        if (cnt >= 5) return 1;
    }
    return 0;
}

// ==================== AI 评估函数 ====================

static int score_line(int cnt, int opens) {
    if (cnt >= 5) return 1000000;       // 连五
    if (cnt == 4) {
        if (opens == 2) return 100000;  // 活四
        if (opens == 1) return 10000;   // 冲四
        return 0;
    }
    if (cnt == 3) {
        if (opens == 2) return 5000;    // 活三
        if (opens == 1) return 500;     // 眠三
        return 0;
    }
    if (cnt == 2) {
        if (opens == 2) return 100;     // 活二
        if (opens == 1) return 10;      // 眠二
        return 0;
    }
    if (cnt == 1 && opens == 2) return 1;
    return 0;
}

static int evaluate_role(int role) {
    static const int dx[4] = {0, 1, 1, 1};
    static const int dy[4] = {1, 0, 1, -1};
    int total = 0;
    for (int i = 0; i < BOARD_W; i++) {
        for (int j = 0; j < BOARD_H; j++) {
            if (s_board[i][j] != role) continue;
            for (int d = 0; d < 4; d++) {
                int pi = i - dx[d], pj = j - dy[d];
                if (pi>=0&&pi<BOARD_W&&pj>=0&&pj<BOARD_H&&s_board[pi][pj]==role) continue;
                int cnt = 0, ni = i, nj = j;
                while (ni>=0&&ni<BOARD_W&&nj>=0&&nj<BOARD_H&&s_board[ni][nj]==role) {
                    cnt++; ni += dx[d]; nj += dy[d];
                }
                int opens = 0;
                if (ni>=0&&ni<BOARD_W&&nj>=0&&nj<BOARD_H&&s_board[ni][nj]==0) opens++;
                if (pi>=0&&pi<BOARD_W&&pj>=0&&pj<BOARD_H&&s_board[pi][pj]==0) opens++;
                total += score_line(cnt, opens);
            }
        }
    }
    return total;
}

static int evaluate_board(int me, int opp) {
    return evaluate_role(me) - evaluate_role(opp) * 11 / 10;
}

// 检测在(x,y)落role子后，某个方向能否形成 cnt+ 连子且 opens 个开放端
// 注意：调用者负责 s_board[x][y] 的赋值与还原
static bool check_pattern(int x, int y, int role, int need_cnt, int need_opens) {
    s_board[x][y] = role;
    bool found = false;
    static const int dx[4] = {0, 1, 1, 1};
    static const int dy[4] = {1, 0, 1, -1};
    for (int d = 0; d < 4 && !found; d++) {
        int cnt = 1, opens = 0;
        // 正方向
        int ni = x + dx[d], nj = y + dy[d];
        while (ni>=0 && ni<BOARD_W && nj>=0 && nj<BOARD_H && s_board[ni][nj]==role) {
            cnt++; ni += dx[d]; nj += dy[d];
        }
        if (ni>=0 && ni<BOARD_W && nj>=0 && nj<BOARD_H && s_board[ni][nj]==0) opens++;
        // 反方向
        ni = x - dx[d]; nj = y - dy[d];
        while (ni>=0 && ni<BOARD_W && nj>=0 && nj<BOARD_H && s_board[ni][nj]==role) {
            cnt++; ni -= dx[d]; nj -= dy[d];
        }
        if (ni>=0 && ni<BOARD_W && nj>=0 && nj<BOARD_H && s_board[ni][nj]==0) opens++;
        if (cnt >= need_cnt && opens >= need_opens) found = true;
    }
    s_board[x][y] = 0;
    return found;
}

// 评估在(x,y)落role子的进攻/防守价值（用于着法排序）
static int score_move(int x, int y, int role) {
    int s = 0;
    if (check_pattern(x, y, role, 5, 0)) s += 1000000;
    if (check_pattern(x, y, role, 4, 2)) s += 100000;
    if (check_pattern(x, y, role, 4, 1)) s += 10000;
    if (check_pattern(x, y, role, 3, 2)) s += 5000;
    if (check_pattern(x, y, role, 3, 1)) s += 500;
    if (check_pattern(x, y, role, 2, 2)) s += 100;
    return s;
}

// 生成候选着法：已有棋子周围 range 格内的空位（按深度 ply 分桶存储）
static void gen_candidates(int range, int ply) {
    s_cand_n[ply] = 0;
    static bool seen[BOARD_W][BOARD_H];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < BOARD_W; i++) {
        for (int j = 0; j < BOARD_H; j++) {
            if (s_board[i][j] == 0) continue;
            for (int dr = -range; dr <= range; dr++) {
                for (int dc = -range; dc <= range; dc++) {
                    int ni = i+dr, nj = j+dc;
                    if (ni<0||ni>=BOARD_W||nj<0||nj>=BOARD_H) continue;
                    if (s_board[ni][nj] != 0 || seen[ni][nj]) continue;
                    seen[ni][nj] = true;
                    s_cand_x[ply][s_cand_n[ply]] = ni;
                    s_cand_y[ply][s_cand_n[ply]] = nj;
                    s_cand_n[ply]++;
                }
            }
        }
    }
    if (s_cand_n[ply] == 0) {
        s_cand_x[ply][0] = BOARD_W / 2; s_cand_y[ply][0] = BOARD_H / 2; s_cand_n[ply] = 1;
    }
}

// α-β 剪枝（按深度 ply 分桶存储候选着法，避免递归覆盖）
static int alphabeta(int depth, int alpha, int beta, int role, int me, int opp, int ply) {
    if (depth == 0) return evaluate_board(me, opp);
    gen_candidates(2, ply);
    if (s_cand_n[ply] == 0) return evaluate_board(me, opp);

    int maximizing = (role == me);
    int best = maximizing ? -2000000 : 2000000;

    for (int k = 0; k < s_cand_n[ply]; k++) {
        int x = s_cand_x[ply][k], y = s_cand_y[ply][k];
        s_board[x][y] = role;
        if (check_win(x, y, role)) {
            s_board[x][y] = 0;
            return maximizing ? 1000000 + depth : -1000000 - depth;
        }
        int val = alphabeta(depth-1, alpha, beta, opp+me-role, me, opp, ply+1);
        s_board[x][y] = 0;
        if (maximizing) {
            if (val > best) best = val;
            if (best > alpha) alpha = best;
        } else {
            if (val < best) best = val;
            if (best < beta) beta = best;
        }
        if (alpha >= beta) break;
    }
    return best;
}

// AI 走棋
static void ai_move(void) {
    int me = s_player;
    int opp = 3 - me;

    gen_candidates(2, 0);  // 根搜索使用 ply=0

    // 1. 我方连五（必胜）
    for (int k = 0; k < s_cand_n[0]; k++) {
        int x = s_cand_x[0][k], y = s_cand_y[0][k];
        s_board[x][y] = me;
        if (check_win(x, y, me)) { s_board[x][y] = 0; s_last_x=x; s_last_y=y; s_board[x][y]=me; return; }
        s_board[x][y] = 0;
    }
    // 2. 对方连五（必挡）
    for (int k = 0; k < s_cand_n[0]; k++) {
        int x = s_cand_x[0][k], y = s_cand_y[0][k];
        if (check_pattern(x, y, opp, 5, 0)) { s_last_x=x; s_last_y=y; s_board[x][y]=me; return; }
    }
    // 3. 我方活四
    for (int k = 0; k < s_cand_n[0]; k++) {
        int x = s_cand_x[0][k], y = s_cand_y[0][k];
        if (check_pattern(x, y, me, 4, 2)) { s_last_x=x; s_last_y=y; s_board[x][y]=me; return; }
    }
    // 4. 对方活四（必挡）
    for (int k = 0; k < s_cand_n[0]; k++) {
        int x = s_cand_x[0][k], y = s_cand_y[0][k];
        if (check_pattern(x, y, opp, 4, 2)) { s_last_x=x; s_last_y=y; s_board[x][y]=me; return; }
    }
    // 5. 对方冲四（必挡）
    for (int k = 0; k < s_cand_n[0]; k++) {
        int x = s_cand_x[0][k], y = s_cand_y[0][k];
        if (check_pattern(x, y, opp, 4, 1)) { s_last_x=x; s_last_y=y; s_board[x][y]=me; return; }
    }
    // 6. 对方活三（必挡）
    for (int k = 0; k < s_cand_n[0]; k++) {
        int x = s_cand_x[0][k], y = s_cand_y[0][k];
        if (check_pattern(x, y, opp, 3, 2)) { s_last_x=x; s_last_y=y; s_board[x][y]=me; return; }
    }

    if (s_diff == GK_EASY) {
        // 简单：威胁检测后随机选一个候选（不搜索）
        int r = rand() % s_cand_n[0];
        int x = s_cand_x[0][r], y = s_cand_y[0][r];
        s_board[x][y] = me; s_last_x=x; s_last_y=y;
        return;
    }

    // 着法排序：按 score_move(me) + score_move(opp) 降序，提升 α-β 剪枝效率
    int n = s_cand_n[0];
    static int score_arr[BOARD_W * BOARD_H];
    for (int k = 0; k < n; k++) {
        int x = s_cand_x[0][k], y = s_cand_y[0][k];
        score_arr[k] = score_move(x, y, me) + score_move(x, y, opp);
    }
    int beam = (s_diff == GK_HARD) ? 12 : 10;
    if (beam > n) beam = n;
    // 选择排序：将分数最高的 beam 个候选交换到前部
    for (int i = 0; i < beam; i++) {
        int max_idx = i;
        for (int j = i + 1; j < n; j++) {
            if (score_arr[j] > score_arr[max_idx]) max_idx = j;
        }
        if (max_idx != i) {
            int tmp = score_arr[i]; score_arr[i] = score_arr[max_idx]; score_arr[max_idx] = tmp;
            tmp = s_cand_x[0][i]; s_cand_x[0][i] = s_cand_x[0][max_idx]; s_cand_x[0][max_idx] = tmp;
            tmp = s_cand_y[0][i]; s_cand_y[0][i] = s_cand_y[0][max_idx]; s_cand_y[0][max_idx] = tmp;
        }
    }

    int depth = (s_diff == GK_HARD) ? 2 : 4;
    int best_val = -2000000, bx = s_cand_x[0][0], by = s_cand_y[0][0];
    int alpha = -2000000, beta = 2000000;

    for (int k = 0; k < beam; k++) {
        int x = s_cand_x[0][k], y = s_cand_y[0][k];
        s_board[x][y] = me;
        int val = alphabeta(depth-1, alpha, beta, opp, me, opp, 1);
        s_board[x][y] = 0;
        if (val > best_val) { best_val = val; bx = x; by = y; }
        if (best_val > alpha) alpha = best_val;
    }
    s_board[bx][by] = me; s_last_x = bx; s_last_y = by;
}

// ==================== 渲染 ====================

static void render_board(void) {
    if (!s_canvas || !s_buf) return;
    lv_canvas_fill_bg(s_canvas, GK_BOARD, LV_OPA_COVER);

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = GK_LINE;
    line_dsc.width = 1;
    line_dsc.opa = LV_OPA_COVER;

    // 网格线
    for (int x = 0; x < BOARD_W; x++) {
        int x_pos = MARGIN_X + x * CELL_SIZE;
        lv_point_t v[2] = {{x_pos, MARGIN_Y}, {x_pos, MARGIN_Y + (BOARD_H-1)*CELL_SIZE}};
        lv_canvas_draw_line(s_canvas, v, 2, &line_dsc);
    }
    for (int y = 0; y < BOARD_H; y++) {
        int y_pos = MARGIN_Y + y * CELL_SIZE;
        lv_point_t h[2] = {{MARGIN_X, y_pos}, {MARGIN_X + (BOARD_W-1)*CELL_SIZE, y_pos}};
        lv_canvas_draw_line(s_canvas, h, 2, &line_dsc);
    }

    // 星位 (小实心圆)
    // 15 列用 {3,7,11}，21 行用 {3,10,17}（居中对称）
    int stars_x[] = {3, 7, 11};
    int stars_y[] = {3, 10, 17};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            int star_x = MARGIN_X + stars_x[i] * CELL_SIZE;
            int star_y = MARGIN_Y + stars_y[j] * CELL_SIZE;
            game_canvas_draw_circle(s_canvas, star_x, star_y, 3, GK_STAR, LV_OPA_COVER);
        }
    }

    // 棋子 (实心圆)
    for (int i = 0; i < BOARD_W; i++) {
        for (int j = 0; j < BOARD_H; j++) {
            if (s_board[i][j] == 0) continue;
            int cx = MARGIN_X + i * CELL_SIZE;
            int cy = MARGIN_Y + j * CELL_SIZE;
            if (s_board[i][j] == 1) {
                game_canvas_draw_circle(s_canvas, cx, cy, STONE_SIZE / 2, GK_BLACK, LV_OPA_COVER);
            } else {
                /* 白棋用描边圆环, 使其在木色棋盘上更清晰 (RGB565 下 set_px 忽略 opa, 不能用半透明覆盖) */
                game_canvas_draw_circle_ring(s_canvas, cx, cy, STONE_SIZE / 2, 1, GK_LINE, GK_WHITE, LV_OPA_COVER);
            }
        }
    }

    // 最后一手标记 (红色圆环)
    if (s_last_x >= 0) {
        int cx = MARGIN_X + s_last_x * CELL_SIZE;
        int cy = MARGIN_Y + s_last_y * CELL_SIZE;
        int role = s_board[s_last_x][s_last_y];
        lv_color_t stone_c = (role == 1) ? GK_BLACK : GK_WHITE;
        game_canvas_draw_circle_ring(s_canvas, cx, cy, STONE_SIZE / 2, 2, GK_LAST, stone_c, LV_OPA_COVER);
    }

    // 待确认落子预览 (提示环, 点"确认落子"后才会真正落子)
    if (s_pending_x >= 0 && s_pending_y >= 0) {
        int cx = MARGIN_X + s_pending_x * CELL_SIZE;
        int cy = MARGIN_Y + s_pending_y * CELL_SIZE;
        game_canvas_draw_circle_ring(s_canvas, cx, cy, STONE_SIZE / 2, 2, GK_INFO, GK_BOARD, LV_OPA_COVER);
    }
}

// ==================== UI ====================

static void ui_refresh(void);

static void canvas_click_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_state != GK_PLAYING) return;
    if (s_mode == GK_PVE && s_player == 2) return; // AI 回合
    if (s_ai_pending) return;

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    // 转换为棋盘坐标
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_point_t local;
    lv_coord_t ox = lv_obj_get_x(obj);
    lv_coord_t oy = lv_obj_get_y(obj);
    local.x = pt.x - ox;
    local.y = pt.y - oy;

    int gx = (local.x - MARGIN_X + CELL_SIZE/2) / CELL_SIZE;
    int gy = (local.y - MARGIN_Y + CELL_SIZE/2) / CELL_SIZE;
    if (gx < 0 || gx >= BOARD_W || gy < 0 || gy >= BOARD_H) return;
    if (s_board[gx][gy] != 0) return;

    // 点击只设置待确认位置(防误触); 再次点击同一位置则取消选择
    if (s_pending_x == gx && s_pending_y == gy) {
        s_pending_x = -1; s_pending_y = -1;
    } else {
        s_pending_x = gx; s_pending_y = gy;
    }
    ui_refresh();
}

// 确认落子: 将待确认位置的棋子真正放到棋盘上
static void confirm_move_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_state != GK_PLAYING) return;
    if (s_pending_x < 0 || s_pending_y < 0) return;
    if (s_mode == GK_PVE && s_player == 2) return; // AI 回合
    if (s_ai_pending) return;

    int gx = s_pending_x, gy = s_pending_y;
    s_pending_x = -1; s_pending_y = -1;
    if (s_board[gx][gy] != 0) return;

    s_board[gx][gy] = s_player;
    s_last_x = gx; s_last_y = gy;
    s_move_count++;
    s_hist_x[s_hist_n] = gx; s_hist_y[s_hist_n] = gy; s_hist_n++;

    if (check_win(gx, gy, s_player)) {
        s_winner = s_player;
        s_state = GK_GAMEOVER;
    } else if (s_move_count >= BOARD_W * BOARD_H) {
        s_winner = 0; // 平局
        s_state = GK_GAMEOVER;
    } else {
        s_player = 3 - s_player;
        if (s_mode == GK_PVE && s_player == 2) s_ai_pending = true;
    }
    ui_refresh();
}

static void new_game_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    board_clear();
    s_state = GK_PLAYING;
    ui_refresh();
}

static void back_menu_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_state = GK_MENU;
    s_pending_x = -1; s_pending_y = -1;
    ui_refresh();
}

static void undo_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_state != GK_PLAYING || s_hist_n == 0) return;
    /* 有待确认位置时, 悔棋键优先取消选择 */
    if (s_pending_x >= 0) {
        s_pending_x = -1; s_pending_y = -1;
        ui_refresh();
        return;
    }
    if (s_ai_pending) return;
    int undo_count = (s_mode == GK_PVE && s_hist_n >= 2) ? 2 : 1;
    for (int u = 0; u < undo_count && s_hist_n > 0; u++) {
        s_hist_n--;
        s_board[s_hist_x[s_hist_n]][s_hist_y[s_hist_n]] = 0;
        s_move_count--;
        s_player = 3 - s_player;
    }
    if (s_hist_n > 0) {
        s_last_x = s_hist_x[s_hist_n - 1];
        s_last_y = s_hist_y[s_hist_n - 1];
    } else {
        s_last_x = -1; s_last_y = -1;
    }
    s_winner = 0;
    ui_refresh();
}

// 菜单回调
static void update_menu_colors(void) {
    for (int i = 0; i < 3; i++)
        if (s_diff_btns[i]) lv_obj_set_style_bg_color(s_diff_btns[i], (s_diff == i) ? GK_INFO : GK_DIM, 0);
    for (int i = 0; i < 2; i++)
        if (s_mode_btns[i]) lv_obj_set_style_bg_color(s_mode_btns[i], (s_mode == i) ? GK_WARN : GK_DIM, 0);
}

static void diff_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_diff = (gk_diff_t)(intptr_t)lv_event_get_user_data(e);
    update_menu_colors();
}
static void mode_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_mode = (gk_mode_t)(intptr_t)lv_event_get_user_data(e);
    update_menu_colors();
}
static void start_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    board_clear();
    s_state = GK_PLAYING;
    ui_refresh();
}

static lv_obj_t *make_btn(lv_obj_t *p, int w, int h, const char *txt, lv_color_t c, lv_event_cb_t cb, void *data) {
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, c, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, CN_FONT, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return b;
}

static void ui_refresh(void) {
    // 状态标签
    if (s_status_lbl) {
        char buf[64];
        if (s_state == GK_MENU) {
            lv_label_set_text(s_status_lbl, "");
        } else if (s_state == GK_PLAYING) {
            if (s_ai_pending) {
                // AI 思考中
                snprintf(buf, sizeof(buf), "AI\xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad...  \xe6\x89\x8b\xe6\x95\xb0:%d", s_move_count);
                lv_label_set_text(s_status_lbl, buf);
            } else {
                const char *turn = (s_player == 1) ?
                    "\xe9\xbb\x91\xe6\xa3\x8b\xe5\x9b\x9e\xe5\x90\x88" :  // 黑棋回合
                    "\xe7\x99\xbd\xe6\xa3\x8b\xe5\x9b\x9e\xe5\x90\x88";   // 白棋回合
                const char *ai = (s_mode == GK_PVE && s_player == 2) ? " (AI)" : "";
                if (s_pending_x >= 0) {
                    snprintf(buf, sizeof(buf), "%s%s  \xe6\x89\x8b\xe6\x95\xb0:%d  \xe7\x82\xb9\xe7\xa1\xae\xe8\xae\xa4\xe8\x90\xbd\xe5\xad\x90",
                             turn, ai, s_move_count);  // 已选位置, 提示点"确认落子"
                } else {
                    snprintf(buf, sizeof(buf), "%s%s  \xe6\x89\x8b\xe6\x95\xb0:%d", turn, ai, s_move_count);
                }
                lv_label_set_text(s_status_lbl, buf);
            }
        } else {
            if (s_winner == 0) lv_label_set_text(s_status_lbl,
                "\xe5\xb9\xb3\xe5\xb1\x80");  // 平局
            else {
                const char *w = (s_winner == 1) ?
                    "\xe9\xbb\x91\xe6\xa3\x8b\xe8\x83\x9c" :  // 黑棋胜
                    "\xe7\x99\xbd\xe6\xa3\x8b\xe8\x83\x9c";   // 白棋胜
                lv_label_set_text(s_status_lbl, w);
            }
        }
    }

    // 面板切换
    if (s_menu_panel) {
        if (s_state == GK_MENU) lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_game_panel) {
        if (s_state != GK_MENU) lv_obj_clear_flag(s_game_panel, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_game_panel, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_state == GK_PLAYING || s_state == GK_GAMEOVER) {
        render_board();
    }
}

static void timer_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    gomoku_update();
    ui_refresh();
}

// ==================== 屏幕卸载 ====================

static void screen_unload_cb(lv_event_t *) {
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    if (s_buf) { heap_caps_free(s_buf); s_buf = NULL; }
    s_screen = NULL;
    s_canvas = NULL;
    s_status_lbl = NULL;
    s_menu_panel = NULL;
    s_game_panel = NULL;
    for (int i = 0; i < 3; i++) s_diff_btns[i] = NULL;
    for (int i = 0; i < 2; i++) s_mode_btns[i] = NULL;
    s_state = GK_MENU;
}

// ==================== 页面创建 ====================

extern "C" void game_gomoku_show(void) {
    board_clear();
    s_state = GK_MENU;

    // PSRAM 画布
    if (!s_buf) {
        s_buf = (lv_color_t *)heap_caps_malloc(
            CANVAS_W * CANVAS_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    if (s_buf) memset(s_buf, 0, CANVAS_W * CANVAS_H * sizeof(lv_color_t));

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, GK_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, screen_unload_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    // 状态标签
    s_status_lbl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_status_lbl, CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_lbl, GK_TEXT, LV_PART_MAIN);
    lv_obj_set_width(s_status_lbl, GAME_W);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 8);

    // ===== 菜单面板 =====
    s_menu_panel = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_menu_panel);
    lv_obj_set_size(s_menu_panel, GAME_W, GAME_H - 20);
    lv_obj_align(s_menu_panel, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_obj_set_style_bg_opa(s_menu_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_menu_panel, 0, 0);
    lv_obj_set_style_border_width(s_menu_panel, 0, 0);
    lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_SCROLLABLE);

    int y = 0;

    // 难度选择
    lv_obj_t *diff_title = lv_label_create(s_menu_panel);
    lv_obj_set_style_text_font(diff_title, CN_FONT, 0);
    lv_obj_set_style_text_color(diff_title, GK_TEXT, 0);
    lv_label_set_text(diff_title, "\xe9\x9a\xbe\xe5\xba\xa6");  // 难度
    lv_obj_align(diff_title, LV_ALIGN_TOP_MID, 0, y);
    y += 30;

    const char *diff_names[] = {
        "\xe7\xae\x80\xe5\x8d\x95",      // 简单
        "\xe5\x9b\xb0\xe9\x9a\xbe",      // 困难
        "\xe8\xb6\x85\xe7\xba\xa7\xe5\xa4\x8d\xe6\x9d\x82"  // 超级复杂
    };
    for (int i = 0; i < 3; i++) {
        lv_color_t c = (s_diff == i) ? GK_INFO : GK_DIM;
        s_diff_btns[i] = make_btn(s_menu_panel, 180, 40, diff_names[i], c, diff_cb, (void*)(intptr_t)i);
        lv_obj_align(s_diff_btns[i], LV_ALIGN_TOP_MID, 0, y);
        y += 48;
    }

    y += 10;

    // 模式选择
    lv_obj_t *mode_title = lv_label_create(s_menu_panel);
    lv_obj_set_style_text_font(mode_title, CN_FONT, 0);
    lv_obj_set_style_text_color(mode_title, GK_TEXT, 0);
    lv_label_set_text(mode_title, "\xe6\xa8\xa1\xe5\xbc\x8f");  // 模式
    lv_obj_align(mode_title, LV_ALIGN_TOP_MID, 0, y);
    y += 30;

    const char *mode_names[] = {
        "\xe4\xba\xba\xe6\x9c\xba\xe5\xaf\xb9\xe6\x88\x98",  // 人机对战
        "\xe5\x8f\x8c\xe4\xba\xba\xe5\xaf\xb9\xe6\x88\x98"    // 双人对战
    };
    for (int i = 0; i < 2; i++) {
        lv_color_t c = (s_mode == i) ? GK_WARN : GK_DIM;
        s_mode_btns[i] = make_btn(s_menu_panel, 180, 40, mode_names[i], c, mode_cb, (void*)(intptr_t)i);
        lv_obj_align(s_mode_btns[i], LV_ALIGN_TOP_MID, 0, y);
        y += 48;
    }

    y += 10;
    lv_obj_t *start_btn = make_btn(s_menu_panel, 180, 44,
        "\xe5\xbc\x80\xe5\xa7\x8b\xe6\xb8\xb8\xe6\x88\x8f",  // 开始游戏
        GK_SUCCESS, start_cb, NULL);
    lv_obj_align(start_btn, LV_ALIGN_TOP_MID, 0, y);

    // ===== 游戏面板 =====
    s_game_panel = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_game_panel);
    lv_obj_set_size(s_game_panel, GAME_W, GAME_H);
    lv_obj_align(s_game_panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(s_game_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_game_panel, 0, 0);
    lv_obj_set_style_border_width(s_game_panel, 0, 0);
    lv_obj_clear_flag(s_game_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_game_panel, LV_OBJ_FLAG_HIDDEN);

    // 棋盘画布 (下移避免与顶部状态文字重叠, 棋盘水平居中)
    if (s_buf) {
        s_canvas = lv_canvas_create(s_game_panel);
        lv_canvas_set_buffer(s_canvas, s_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(s_canvas, LV_ALIGN_TOP_MID, 0, 32);
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_canvas, canvas_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // 底部按钮
    int btn_y = 32 + CANVAS_H + 8;
    lv_obj_t *new_btn = make_btn(s_game_panel, 80, 32,
        "\xe6\x96\xb0\xe5\xb1\x80", GK_INFO, new_game_cb, NULL);  // 新局
    lv_obj_align(new_btn, LV_ALIGN_TOP_MID, -90, btn_y);

    lv_obj_t *undo_btn = make_btn(s_game_panel, 80, 32,
        "\xe6\x82\x94\xe6\xa3\x8b", GK_WARN, undo_cb, NULL);  // 悔棋
    lv_obj_align(undo_btn, LV_ALIGN_TOP_MID, 0, btn_y);

    lv_obj_t *menu_btn = make_btn(s_game_panel, 80, 32,
        "\xe8\x8f\x9c\xe5\x8d\x95", GK_DANGER, back_menu_cb, NULL);  // 菜单
    lv_obj_align(menu_btn, LV_ALIGN_TOP_MID, 90, btn_y);

    // 确认落子按钮 (防止误触: 先点棋盘选位置, 再点此按钮真正落子)
    lv_obj_t *confirm_btn = make_btn(s_game_panel, 150, 36,
        "\xe7\xa1\xae\xe8\xae\xa4\xe8\x90\xbd\xe5\xad\x90", GK_SUCCESS, confirm_move_cb, NULL);  // 确认落子
    lv_obj_align(confirm_btn, LV_ALIGN_TOP_MID, 0, btn_y + 44);

    s_timer = lv_timer_create(timer_cb, REFRESH_MS, NULL);
    ui_navbar_push_screen_named(s_screen, true, "五子棋");
    ui_refresh();
}

// ==================== 主循环更新 ====================

void gomoku_update(void) {
    if (!s_screen || lv_screen_active() != s_screen) return;
    if (s_state != GK_PLAYING) return;
    if (!s_ai_pending) return;

    // AI 走棋
    ai_move();
    s_move_count++;
    s_hist_x[s_hist_n] = s_last_x; s_hist_y[s_hist_n] = s_last_y; s_hist_n++;

    if (check_win(s_last_x, s_last_y, s_player)) {
        s_winner = s_player;
        s_state = GK_GAMEOVER;
    } else if (s_move_count >= BOARD_W * BOARD_H) {
        s_winner = 0;
        s_state = GK_GAMEOVER;
    } else {
        s_player = 3 - s_player;
    }
    s_ai_pending = false;
    ui_refresh();
}
