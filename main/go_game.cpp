#include "go_game.h"
#include "ui_navbar.h"
#include "game_canvas_compat.h"
#include "bsp_board.h"
#include "bsp_theme.h"
#include "esp_2_41ttf.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "lvgl.h"
#include <stdio.h>

#define GAME_W MIPI_DSI_LCD_H_RES
#define GAME_H MIPI_DSI_LCD_V_RES
#define CN_FONT (&esp_2)
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ==================== 围棋游戏 (9x9) ====================
// 9x9 棋盘，黑棋先手
// 规则：气、提子、劫、禁着（自杀）、双虚手终局
// 中国规则计分（地+子），白方贴目 7 目
// AI 三档难度：
//   简单：随机 + 优先提子
//   困难：2 层 minimax + α-β 剪枝
//   超级：3 层 minimax + 候选剪枝 + 战术检查

#define GO_SIZE      9
#define GO_CANVAS_W  262
#define GO_CANVAS_H  262
#define GO_CELL      28
#define GO_MARGIN    19
#define GO_STONE_R   10
#define GO_REFRESH   50
#define GO_KOMI      7    // 白方贴目

// 颜色
#define GO_BG        lv_color_hex(0x0A0A0F)
#define GO_BOARD     lv_color_hex(0xD4A76A)
#define GO_LINE      lv_color_hex(0x3D2B1F)
#define GO_BLACK     lv_color_hex(0x1A1A1A)
#define GO_WHITE     lv_color_hex(0xF5F5F5)
#define GO_LAST      lv_color_hex(0xE53E3E)
#define GO_STAR      lv_color_hex(0x2D1810)
#define GO_KO        lv_color_hex(0x8B5CF6)
#define GO_TEXT      lv_color_hex(0xE5E7EB)
#define GO_DIM       lv_color_hex(0x6B7280)
#define GO_INFO      lv_color_hex(0x3B82F6)
#define GO_SUCCESS   lv_color_hex(0x10B981)
#define GO_WARN      lv_color_hex(0xF59E0B)
#define GO_DANGER    lv_color_hex(0xDC2626)

// 状态
typedef enum { GO_MENU, GO_PLAYING, GO_GAMEOVER } go_state_t;
typedef enum { GO_EASY, GO_HARD, GO_SUPER } go_diff_t;
typedef enum { GO_PVE, GO_PVP } go_mode_t;

// 全局状态
static go_state_t s_state = GO_MENU;
static go_diff_t s_diff = GO_HARD;
static go_mode_t s_mode = GO_PVE;
static int s_board[GO_SIZE][GO_SIZE];        // 0=空, 1=黑, 2=白
static int s_player = 1;                     // 1=黑, 2=白
static int s_last_x = -1, s_last_y = -1;     // 最后一手
static int s_move_count = 0;
static int s_pass_count = 0;
static int s_capt_b = 0;                     // 黑方提走的白子数
static int s_capt_w = 0;                     // 白方提走的黑子数
static int s_ko_x = -1, s_ko_y = -1;         // 劫点（禁着）
static bool s_ai_pending = false;
static int s_score_b = 0, s_score_w = 0;     // 终局得分

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_canvas = NULL;
static lv_color_t *s_buf = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_menu_panel = NULL;
static lv_obj_t *s_game_panel = NULL;
static lv_timer_t *s_timer = NULL;

// AI 候选着法
static int s_cand_x[81], s_cand_y[81], s_cand_n;

// 菜单按钮引用
static lv_obj_t *s_diff_btns[3] = {NULL};
static lv_obj_t *s_mode_btns[2] = {NULL};

// ==================== 快照（用于搜索和悔棋） ====================

typedef struct {
    int board[GO_SIZE][GO_SIZE];
    int ko_x, ko_y;
    int capt_b, capt_w;
} go_snapshot_t;

static void save_snapshot(go_snapshot_t *s) {
    memcpy(s->board, s_board, sizeof(s_board));
    s->ko_x = s_ko_x;
    s->ko_y = s_ko_y;
    s->capt_b = s_capt_b;
    s->capt_w = s_capt_w;
}

static void restore_snapshot(const go_snapshot_t *s) {
    memcpy(s_board, s->board, sizeof(s_board));
    s_ko_x = s->ko_x;
    s_ko_y = s->ko_y;
    s_capt_b = s->capt_b;
    s_capt_w = s->capt_w;
}

// 悔棋历史
#define GO_HIST_MAX 90
typedef struct {
    go_snapshot_t snap;
    int player;
    int last_x, last_y;
    int move_count;
    int pass_count;
} go_hist_t;
static go_hist_t s_hist[GO_HIST_MAX] EXT_RAM_BSS_ATTR;  // ~30KB, 放 PSRAM BSS 省 internal RAM
static int s_hist_n = 0;

static void hist_push(void) {
    if (s_hist_n >= GO_HIST_MAX) {
        memmove(s_hist, s_hist + 1, (GO_HIST_MAX - 1) * sizeof(go_hist_t));
        s_hist_n = GO_HIST_MAX - 1;
    }
    save_snapshot(&s_hist[s_hist_n].snap);
    s_hist[s_hist_n].player = s_player;
    s_hist[s_hist_n].last_x = s_last_x;
    s_hist[s_hist_n].last_y = s_last_y;
    s_hist[s_hist_n].move_count = s_move_count;
    s_hist[s_hist_n].pass_count = s_pass_count;
    s_hist_n++;
}

static bool hist_pop(void) {
    if (s_hist_n == 0) return false;
    s_hist_n--;
    restore_snapshot(&s_hist[s_hist_n].snap);
    s_player = s_hist[s_hist_n].player;
    s_last_x = s_hist[s_hist_n].last_x;
    s_last_y = s_hist[s_hist_n].last_y;
    s_move_count = s_hist[s_hist_n].move_count;
    s_pass_count = s_hist[s_hist_n].pass_count;
    return true;
}

// ==================== 棋盘逻辑 ====================

static void board_clear(void) {
    memset(s_board, 0, sizeof(s_board));
    s_player = 1;
    s_last_x = -1; s_last_y = -1;
    s_move_count = 0;
    s_pass_count = 0;
    s_capt_b = 0;
    s_capt_w = 0;
    s_ko_x = -1; s_ko_y = -1;
    s_ai_pending = false;
    s_hist_n = 0;
    s_score_b = 0; s_score_w = 0;
}

// 洪泛求一组连通同色棋子的气数和组大小
// 工作数组通过参数传入（栈上），保证 minimax 递归时不会被覆盖
static int flood_group(int x, int y, int role, bool *visited, int *group_size,
                       int *qx_buf, int *qy_buf, bool *lib_marked_buf) {
    static const int dx[4] = {0, 0, 1, -1};
    static const int dy[4] = {1, -1, 0, 0};
    int head = 0, tail = 0;
    qx_buf[tail] = x; qy_buf[tail] = y; tail++;
    visited[x * GO_SIZE + y] = true;
    int gsize = 0;
    int libs = 0;
    memset(lib_marked_buf, 0, GO_SIZE * GO_SIZE * sizeof(bool));

    while (head < tail) {
        int cx = qx_buf[head], cy = qy_buf[head];
        head++;
        gsize++;
        for (int d = 0; d < 4; d++) {
            int nx = cx + dx[d], ny = cy + dy[d];
            if (nx < 0 || nx >= GO_SIZE || ny < 0 || ny >= GO_SIZE) continue;
            int idx = nx * GO_SIZE + ny;
            if (s_board[nx][ny] == 0) {
                if (!lib_marked_buf[idx]) {
                    lib_marked_buf[idx] = true;
                    libs++;
                }
            } else if (s_board[nx][ny] == role && !visited[idx]) {
                visited[idx] = true;
                qx_buf[tail] = nx; qy_buf[tail] = ny; tail++;
            }
        }
    }
    if (group_size) *group_size = gsize;
    return libs;
}

// 移除一组连通同色棋子，返回移除数量
static int remove_group(int x, int y, int role) {
    static const int dx[4] = {0, 0, 1, -1};
    static const int dy[4] = {1, -1, 0, 0};
    static int qx[81], qy[81];
    int head = 0, tail = 0;
    bool visited[81] = {false};
    qx[tail] = x; qy[tail] = y; tail++;
    visited[x * GO_SIZE + y] = true;
    int removed = 0;
    while (head < tail) {
        int cx = qx[head], cy = qy[head];
        head++;
        s_board[cx][cy] = 0;
        removed++;
        for (int d = 0; d < 4; d++) {
            int nx = cx + dx[d], ny = cy + dy[d];
            if (nx < 0 || nx >= GO_SIZE || ny < 0 || ny >= GO_SIZE) continue;
            int idx = nx * GO_SIZE + ny;
            if (s_board[nx][ny] == role && !visited[idx]) {
                visited[idx] = true;
                qx[tail] = nx; qy[tail] = ny; tail++;
            }
        }
    }
    return removed;
}

// 执行落子（更新棋盘、提子、劫点、提子数）
// 返回提走的对方棋子数
static int do_play(int x, int y, int role) {
    s_ko_x = -1; s_ko_y = -1;  // 任何落子清除劫点
    s_board[x][y] = role;
    int opp = 3 - role;
    static const int dx[4] = {0, 0, 1, -1};
    static const int dy[4] = {1, -1, 0, 0};
    int captured = 0;
    int cap_x = -1, cap_y = -1;

    for (int d = 0; d < 4; d++) {
        int nx = x + dx[d], ny = y + dy[d];
        if (nx < 0 || nx >= GO_SIZE || ny < 0 || ny >= GO_SIZE) continue;
        if (s_board[nx][ny] != opp) continue;
        bool visited[81] = {false};
        int qx[81], qy[81];
        bool lm[81] = {false};
        int libs = flood_group(nx, ny, opp, visited, NULL, qx, qy, lm);
        if (libs == 0) {
            int r = remove_group(nx, ny, opp);
            captured += r;
            if (r == 1) { cap_x = nx; cap_y = ny; }
        }
    }

    // 劫检测：恰好提走 1 子，且落子单子且只有 1 气
    if (captured == 1) {
        bool visited[81] = {false};
        int qx[81], qy[81];
        bool lm[81] = {false};
        int gsize = 0;
        int my_libs = flood_group(x, y, role, visited, &gsize, qx, qy, lm);
        if (gsize == 1 && my_libs == 1) {
            s_ko_x = cap_x;
            s_ko_y = cap_y;
        }
    }

    if (role == 1) s_capt_b += captured;
    else s_capt_w += captured;
    return captured;
}

// 检查落子是否合法（不实际修改全局状态，内部做临时修改并恢复）
static bool is_legal(int x, int y, int role) {
    if (x < 0 || x >= GO_SIZE || y < 0 || y >= GO_SIZE) return false;
    if (s_board[x][y] != 0) return false;
    if (x == s_ko_x && y == s_ko_y) return false;  // 劫禁着

    int backup[GO_SIZE][GO_SIZE];
    memcpy(backup, s_board, sizeof(s_board));

    s_board[x][y] = role;
    int opp = 3 - role;
    static const int dx[4] = {0, 0, 1, -1};
    static const int dy[4] = {1, -1, 0, 0};
    for (int d = 0; d < 4; d++) {
        int nx = x + dx[d], ny = y + dy[d];
        if (nx < 0 || nx >= GO_SIZE || ny < 0 || ny >= GO_SIZE) continue;
        if (s_board[nx][ny] != opp) continue;
        bool visited[81] = {false};
        int qx[81], qy[81];
        bool lm[81] = {false};
        int libs = flood_group(nx, ny, opp, visited, NULL, qx, qy, lm);
        if (libs == 0) {
            remove_group(nx, ny, opp);
        }
    }

    // 自杀检测：提子后若仍无气则非法
    bool visited[81] = {false};
    int qx[81], qy[81];
    bool lm[81] = {false};
    int my_libs = flood_group(x, y, role, visited, NULL, qx, qy, lm);
    bool legal = (my_libs > 0);

    memcpy(s_board, backup, sizeof(s_board));
    return legal;
}

// ==================== 计分（中国规则：地+子） ====================

static void calculate_score(void) {
    s_score_b = 0;
    s_score_w = GO_KOMI;
    bool visited[81] = {false};

    for (int i = 0; i < GO_SIZE; i++) {
        for (int j = 0; j < GO_SIZE; j++) {
            if (s_board[i][j] == 1) s_score_b++;
            else if (s_board[i][j] == 2) s_score_w++;
            else if (!visited[i * GO_SIZE + j]) {
                // 空点洪泛，判断归属
                static int qx[81], qy[81];
                int head = 0, tail = 0;
                qx[tail] = i; qy[tail] = j; tail++;
                visited[i * GO_SIZE + j] = true;
                int region = 0;
                bool touch_b = false, touch_w = false;
                static const int dx[4] = {0, 0, 1, -1};
                static const int dy[4] = {1, -1, 0, 0};
                while (head < tail) {
                    int cx = qx[head], cy = qy[head]; head++;
                    region++;
                    for (int d = 0; d < 4; d++) {
                        int nx = cx + dx[d], ny = cy + dy[d];
                        if (nx < 0 || nx >= GO_SIZE || ny < 0 || ny >= GO_SIZE) continue;
                        if (s_board[nx][ny] == 1) touch_b = true;
                        else if (s_board[nx][ny] == 2) touch_w = true;
                        else if (!visited[nx * GO_SIZE + ny]) {
                            visited[nx * GO_SIZE + ny] = true;
                            qx[tail] = nx; qy[tail] = ny; tail++;
                        }
                    }
                }
                if (touch_b && !touch_w) s_score_b += region;
                else if (touch_w && !touch_b) s_score_w += region;
            }
        }
    }
}

// ==================== AI ====================

// 生成候选着法：已有棋子 range 格内的空位
static void gen_candidates(int range) {
    s_cand_n = 0;
    static bool seen[GO_SIZE][GO_SIZE];
    memset(seen, 0, sizeof(seen));
    bool has_stone = false;

    for (int i = 0; i < GO_SIZE; i++) {
        for (int j = 0; j < GO_SIZE; j++) {
            if (s_board[i][j] == 0) continue;
            has_stone = true;
            for (int dr = -range; dr <= range; dr++) {
                for (int dc = -range; dc <= range; dc++) {
                    int ni = i + dr, nj = j + dc;
                    if (ni < 0 || ni >= GO_SIZE || nj < 0 || nj >= GO_SIZE) continue;
                    if (s_board[ni][nj] != 0 || seen[ni][nj]) continue;
                    seen[ni][nj] = true;
                    s_cand_x[s_cand_n] = ni;
                    s_cand_y[s_cand_n] = nj;
                    s_cand_n++;
                }
            }
        }
    }

    if (!has_stone || s_cand_n == 0) {
        // 开局或无候选：星位
        int stars[] = {2, 4, 6};
        for (int i = 0; i < 3 && s_cand_n < 81; i++)
            for (int j = 0; j < 3 && s_cand_n < 81; j++) {
                s_cand_x[s_cand_n] = stars[i];
                s_cand_y[s_cand_n] = stars[j];
                s_cand_n++;
            }
    }
}

// 影响图：用于地盘评估
static int8_t s_infl[GO_SIZE][GO_SIZE];

static void compute_influence(void) {
    memset(s_infl, 0, sizeof(s_infl));
    static const int dx[8] = {-1,-1,-1,0,0,1,1,1};
    static const int dy[8] = {-1,0,1,-1,1,-1,0,1};
    for (int i = 0; i < GO_SIZE; i++) {
        for (int j = 0; j < GO_SIZE; j++) {
            int c = s_board[i][j];
            if (c == 0) continue;
            int sign = (c == 1) ? +1 : -1;
            s_infl[i][j] += sign * 8;
            for (int d = 0; d < 8; d++) {
                for (int dist = 1; dist <= 4; dist++) {
                    int ni = i + dx[d]*dist, nj = j + dy[d]*dist;
                    if (ni < 0 || ni >= GO_SIZE || nj < 0 || nj >= GO_SIZE) break;
                    if (s_board[ni][nj] != 0 && s_board[ni][nj] != c) break;
                    if (s_board[ni][nj] == c) continue;
                    s_infl[ni][nj] += sign * (5 - dist);
                }
            }
        }
    }
}

// 局面评估：子力 + 气 + 打吃 + 地盘
static int evaluate_position(int me, int opp) {
    int my_stones = 0, opp_stones = 0;
    int my_libs = 0, opp_libs = 0;
    int my_atari = 0, opp_atari = 0;

    bool visited[81] = {false};
    for (int i = 0; i < GO_SIZE; i++) {
        for (int j = 0; j < GO_SIZE; j++) {
            if (s_board[i][j] == 0 || visited[i * GO_SIZE + j]) continue;
            int qx[81], qy[81], gsize = 0;
            bool lm[81] = {false};
            int libs = flood_group(i, j, s_board[i][j], visited, &gsize, qx, qy, lm);
            if (s_board[i][j] == me) {
                my_stones += gsize;
                my_libs += libs;
                if (libs == 1) my_atari += gsize;
            } else {
                opp_stones += gsize;
                opp_libs += libs;
                if (libs == 1) opp_atari += gsize;
            }
        }
    }

    // 地盘评估
    compute_influence();
    int my_ter = 0, opp_ter = 0;
    for (int i = 0; i < GO_SIZE; i++)
        for (int j = 0; j < GO_SIZE; j++) {
            if (s_board[i][j] != 0) continue;
            int v = s_infl[i][j];
            if (me == 1) {
                if (v >= 4) my_ter++;
                else if (v <= -4) opp_ter++;
            } else {
                if (v >= 4) opp_ter++;
                else if (v <= -4) my_ter++;
            }
        }

    // 加权汇总
    int score = 0;
    score += (my_stones - opp_stones) * 4;
    score += (my_libs - opp_libs) * 1;
    score += (opp_atari - my_atari) * 8;
    score += (my_ter - opp_ter) * 6;

    // 开局位置奖励（前15手）
    if (s_move_count < 15) {
        for (int i = 0; i < GO_SIZE; i++)
            for (int j = 0; j < GO_SIZE; j++) {
                if (s_board[i][j] != me) continue;
                int de = i; if (de > 4) de = 8 - i;
                int dj = j; if (dj > 4) dj = 8 - j;
                int line = (de < dj) ? de : dj;
                if (line == 0) score -= 6;
                else if (line == 1) score -= 2;
                else if (line == 2) score += 4;
                else if (line == 3) score += 5;
                else score += 2;
            }
    }
    return score;
}

// minimax + α-β 剪枝
static int minimax(int depth, int alpha, int beta, int role, int me, int opp) {
    if (depth == 0) return evaluate_position(me, opp);
    gen_candidates(2);
    if (s_cand_n == 0) return evaluate_position(me, opp);

    int max_cand = (depth >= 3) ? 12 : s_cand_n;
    if (max_cand > s_cand_n) max_cand = s_cand_n;
    // 复制候选到栈上，防止递归 gen_candidates 覆盖全局 s_cand
    int cx[81], cy[81];
    for (int k = 0; k < max_cand; k++) {
        cx[k] = s_cand_x[k];
        cy[k] = s_cand_y[k];
    }
    int maximizing = (role == me);
    int best = maximizing ? -1000000 : 1000000;

    for (int k = 0; k < max_cand; k++) {
        int x = cx[k], y = cy[k];
        if (!is_legal(x, y, role)) continue;

        go_snapshot_t snap;
        save_snapshot(&snap);
        do_play(x, y, role);
        int val = minimax(depth - 1, alpha, beta, 3 - role, me, opp);
        restore_snapshot(&snap);

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

// 判断是否为己方真眼（禁止自填眼）
static bool is_real_eye(int x, int y, int me) {
    if (s_board[x][y] != 0) return false;
    static const int dx[4] = {0, 0, 1, -1};
    static const int dy[4] = {1, -1, 0, 0};
    int edge = 0, friendly = 0;
    for (int d = 0; d < 4; d++) {
        int nx = x + dx[d], ny = y + dy[d];
        if (nx < 0 || nx >= GO_SIZE || ny < 0 || ny >= GO_SIZE) { edge++; continue; }
        if (s_board[nx][ny] == me) friendly++;
        else return false;
    }
    if (friendly + edge < 4) return false;
    // 对角检查
    static const int dx2[4] = {-1, -1, 1, 1};
    static const int dy2[4] = {-1, 1, -1, 1};
    int bad = 0;
    for (int d = 0; d < 4; d++) {
        int nx = x + dx2[d], ny = y + dy2[d];
        if (nx < 0 || nx >= GO_SIZE || ny < 0 || ny >= GO_SIZE) continue;
        if (s_board[nx][ny] != me && s_board[nx][ny] != 0) bad++;
    }
    if (edge > 0) return bad == 0;
    return bad <= 1;
}

// 候选着法快速评分（用于排序）
static int quick_score(int x, int y, int me) {
    int s = 0;
    int opp = 3 - me;
    static const int dx[4] = {0, 0, 1, -1};
    static const int dy[4] = {1, -1, 0, 0};
    for (int d = 0; d < 4; d++) {
        int nx = x + dx[d], ny = y + dy[d];
        if (nx < 0 || nx >= GO_SIZE || ny < 0 || ny >= GO_SIZE) continue;
        if (s_board[nx][ny] == opp) s += 3;
        else if (s_board[nx][ny] == me) s += 2;
    }
    // 中心偏好
    int cx = 4, cy = 4;
    int dist = abs(x - cx) + abs(y - cy);
    s += (8 - dist);
    return s;
}

// AI 走棋，返回 true=落子 false=虚手
static bool ai_move(void) {
    int me = s_player;
    int opp = 3 - me;

    gen_candidates(2);

    // 过滤合法着法 + 禁止自填眼
    int legal_x[81], legal_y[81], legal_n = 0;
    for (int k = 0; k < s_cand_n; k++) {
        int x = s_cand_x[k], y = s_cand_y[k];
        if (!is_legal(x, y, me)) continue;
        if (is_real_eye(x, y, me)) continue;  // 不自填眼
        legal_x[legal_n] = x;
        legal_y[legal_n] = y;
        legal_n++;
    }
    if (legal_n == 0) return false;  // 无合法着法，虚手

    // ===== 战术检查：能提子就提（所有难度） =====
    int best_cap = 0, cap_bx = -1, cap_by = -1;
    for (int k = 0; k < legal_n; k++) {
        go_snapshot_t snap;
        save_snapshot(&snap);
        int cap = do_play(legal_x[k], legal_y[k], me);
        restore_snapshot(&snap);
        if (cap > best_cap) {
            best_cap = cap;
            cap_bx = legal_x[k];
            cap_by = legal_y[k];
        }
    }

    // ===== 简单难度：只用 quick_score，提子优先 =====
    if (s_diff == GO_EASY) {
        if (best_cap > 0) {
            do_play(cap_bx, cap_by, me);
            s_last_x = cap_bx; s_last_y = cap_by;
            return true;
        }
        int best_qs = -1000000, bx = legal_x[0], by = legal_y[0];
        for (int k = 0; k < legal_n; k++) {
            int qs = quick_score(legal_x[k], legal_y[k], me);
            if (qs > best_qs) {
                best_qs = qs;
                bx = legal_x[k];
                by = legal_y[k];
            }
        }
        do_play(bx, by, me);
        s_last_x = bx; s_last_y = by;
        return true;
    }

    // ===== 困难/超级：minimax 搜索 =====
    int depth = (s_diff == GO_HARD) ? 2 : 3;
    int beam = (s_diff == GO_HARD) ? 15 : 10;

    // 战术提子优先（提子数 >= 2 直接走）
    if (best_cap >= 2) {
        do_play(cap_bx, cap_by, me);
        s_last_x = cap_bx; s_last_y = cap_by;
        return true;
    }

    // 用 quick_score 对候选排序（降序），只取前 beam 个搜索
    int qs_arr[81], idx[81];
    for (int k = 0; k < legal_n; k++) {
        qs_arr[k] = quick_score(legal_x[k], legal_y[k], me);
        idx[k] = k;
    }
    for (int i = 1; i < legal_n; i++) {
        int j = i;
        while (j > 0 && qs_arr[idx[j - 1]] < qs_arr[idx[j]]) {
            int t = idx[j - 1]; idx[j - 1] = idx[j]; idx[j] = t;
            j--;
        }
    }

    int search_n = (legal_n < beam) ? legal_n : beam;
    int best_val = -1000000, bx = legal_x[idx[0]], by = legal_y[idx[0]];
    int alpha = -1000000, beta = 1000000;

    for (int k = 0; k < search_n; k++) {
        int x = legal_x[idx[k]], y = legal_y[idx[k]];
        go_snapshot_t snap;
        save_snapshot(&snap);
        do_play(x, y, me);
        int val = minimax(depth - 1, alpha, beta, opp, me, opp);
        restore_snapshot(&snap);
        // 加上提子奖励
        if (x == cap_bx && y == cap_by) val += best_cap * 8;
        if (val > best_val) {
            best_val = val;
            bx = x;
            by = y;
        }
        if (best_val > alpha) alpha = best_val;
    }

    // 残局且劣势时虚手
    if (best_val < -80 && s_move_count > 55) return false;

    do_play(bx, by, me);
    s_last_x = bx; s_last_y = by;
    return true;
}

// ==================== 渲染 ====================

static void render_board(void) {
    if (!s_canvas || !s_buf) return;
    lv_canvas_fill_bg(s_canvas, GO_BOARD, LV_OPA_COVER);

    // 网格线
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = GO_LINE;
    line_dsc.width = 1;
    line_dsc.opa = LV_OPA_COVER;
    for (int i = 0; i < GO_SIZE; i++) {
        int pos = GO_MARGIN + i * GO_CELL;
        lv_point_t h[2] = {{GO_MARGIN, pos}, {GO_MARGIN + (GO_SIZE - 1) * GO_CELL, pos}};
        lv_point_t v[2] = {{pos, GO_MARGIN}, {pos, GO_MARGIN + (GO_SIZE - 1) * GO_CELL}};
        lv_canvas_draw_line(s_canvas, h, 2, &line_dsc);
        lv_canvas_draw_line(s_canvas, v, 2, &line_dsc);
    }

    // 星位（9x9: 2,4,6 行列的交叉点，共 5 个）
    lv_draw_rect_dsc_t star_dsc;
    lv_draw_rect_dsc_init(&star_dsc);
    star_dsc.bg_color = GO_STAR;
    star_dsc.bg_opa = LV_OPA_COVER;
    star_dsc.radius = 3;
    int stars[] = {2, 4, 6};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            if ((i == 1 && j == 1) || (i != 1 && j != 1)) {
                // 中心 (4,4) 和四角 (2,2)(2,6)(6,2)(6,6)
                lv_canvas_draw_rect(s_canvas,
                    GO_MARGIN + stars[i] * GO_CELL - 3,
                    GO_MARGIN + stars[j] * GO_CELL - 3, 6, 6, &star_dsc);
            }
        }

    // 棋子
    lv_draw_rect_dsc_t stone_dsc;
    lv_draw_rect_dsc_init(&stone_dsc);
    stone_dsc.bg_opa = LV_OPA_COVER;
    stone_dsc.radius = LV_RADIUS_CIRCLE;
    stone_dsc.border_width = 1;

    for (int i = 0; i < GO_SIZE; i++) {
        for (int j = 0; j < GO_SIZE; j++) {
            if (s_board[i][j] == 0) continue;
            int cx = GO_MARGIN + i * GO_CELL;
            int cy = GO_MARGIN + j * GO_CELL;
            stone_dsc.bg_color = (s_board[i][j] == 1) ? GO_BLACK : GO_WHITE;
            stone_dsc.border_color = GO_LINE;
            lv_canvas_draw_rect(s_canvas, cx - GO_STONE_R, cy - GO_STONE_R,
                                GO_STONE_R * 2, GO_STONE_R * 2, &stone_dsc);
        }
    }

    // 最后一手标记
    if (s_last_x >= 0 && s_last_x < GO_SIZE && s_last_y >= 0 && s_last_y < GO_SIZE) {
        int cx = GO_MARGIN + s_last_x * GO_CELL;
        int cy = GO_MARGIN + s_last_y * GO_CELL;
        lv_draw_rect_dsc_t mark_dsc;
        lv_draw_rect_dsc_init(&mark_dsc);
        mark_dsc.bg_opa = LV_OPA_TRANSP;
        mark_dsc.border_color = GO_LAST;
        mark_dsc.border_width = 2;
        mark_dsc.radius = LV_RADIUS_CIRCLE;
        lv_canvas_draw_rect(s_canvas, cx - 5, cy - 5, 10, 10, &mark_dsc);
    }

    // 劫点标记
    if (s_ko_x >= 0 && s_ko_y >= 0) {
        int cx = GO_MARGIN + s_ko_x * GO_CELL;
        int cy = GO_MARGIN + s_ko_y * GO_CELL;
        lv_draw_rect_dsc_t ko_dsc;
        lv_draw_rect_dsc_init(&ko_dsc);
        ko_dsc.bg_opa = LV_OPA_TRANSP;
        ko_dsc.border_color = GO_KO;
        ko_dsc.border_width = 2;
        ko_dsc.radius = LV_RADIUS_CIRCLE;
        lv_canvas_draw_rect(s_canvas, cx - 8, cy - 8, 16, 16, &ko_dsc);
    }
}

// ==================== UI ====================

static void ui_refresh(void);

static void canvas_click_cb(lv_event_t *e) {
    if (s_state != GO_PLAYING) return;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_ai_pending) return;
    if (s_mode == GO_PVE && s_player == 2) return;  // AI 回合

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    /* 目标在 game_panel(偏移 UI_NAVBAR_H) 内, 需累加父级偏移得到屏幕坐标 */
    lv_coord_t ox = lv_obj_get_x(obj);
    lv_coord_t oy = lv_obj_get_y(obj);
    lv_obj_t *parent = lv_obj_get_parent(obj);
    if (parent) {
        ox += lv_obj_get_x(parent);
        oy += lv_obj_get_y(parent);
    }
    int local_x = pt.x - ox;
    int local_y = pt.y - oy;

    int gx = (local_x - GO_MARGIN + GO_CELL / 2) / GO_CELL;
    int gy = (local_y - GO_MARGIN + GO_CELL / 2) / GO_CELL;
    if (gx < 0 || gx >= GO_SIZE || gy < 0 || gy >= GO_SIZE) return;
    if (!is_legal(gx, gy, s_player)) return;

    hist_push();
    do_play(gx, gy, s_player);
    s_last_x = gx; s_last_y = gy;
    s_move_count++;
    s_pass_count = 0;

    if (s_move_count >= GO_SIZE * GO_SIZE) {
        calculate_score();
        s_state = GO_GAMEOVER;
    } else {
        s_player = 3 - s_player;
        if (s_mode == GO_PVE && s_player == 2) s_ai_pending = true;
    }
    ui_refresh();
}

static void new_game_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    board_clear();
    s_state = GO_PLAYING;
    ui_refresh();
}

static void back_menu_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_state = GO_MENU;
    ui_refresh();
}

static void pass_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_state != GO_PLAYING || s_ai_pending) return;
    if (s_mode == GO_PVE && s_player == 2) return;

    hist_push();
    s_pass_count++;
    s_last_x = -1; s_last_y = -1;

    if (s_pass_count >= 2) {
        calculate_score();
        s_state = GO_GAMEOVER;
    } else {
        s_player = 3 - s_player;
        if (s_mode == GO_PVE && s_player == 2) s_ai_pending = true;
    }
    ui_refresh();
}

static void undo_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_state == GO_MENU) return;
    if (s_ai_pending) return;
    // PvE 撤回 2 步（AI + 玩家），PvP 撤回 1 步
    int undo_n = (s_mode == GO_PVE && s_hist_n >= 2) ? 2 : 1;
    for (int i = 0; i < undo_n && s_hist_n > 0; i++) {
        hist_pop();
    }
    if (s_state == GO_GAMEOVER) s_state = GO_PLAYING;
    ui_refresh();
}

static void update_menu_colors(void) {
    for (int i = 0; i < 3; i++)
        if (s_diff_btns[i]) lv_obj_set_style_bg_color(s_diff_btns[i], (s_diff == i) ? GO_INFO : GO_DIM, 0);
    for (int i = 0; i < 2; i++)
        if (s_mode_btns[i]) lv_obj_set_style_bg_color(s_mode_btns[i], (s_mode == i) ? GO_WARN : GO_DIM, 0);
}

static void diff_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_diff = (go_diff_t)(intptr_t)lv_event_get_user_data(e);
    update_menu_colors();
}
static void mode_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_mode = (go_mode_t)(intptr_t)lv_event_get_user_data(e);
    update_menu_colors();
}
static void start_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    board_clear();
    s_state = GO_PLAYING;
    ui_refresh();
}

static lv_obj_t *make_btn(lv_obj_t *p, int w, int h, const char *txt,
                          lv_color_t c, lv_event_cb_t cb, void *data) {
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
        char buf[128];
        if (s_state == GO_MENU) {
            lv_label_set_text(s_status_lbl, "");
        } else if (s_state == GO_PLAYING) {
            if (s_ai_pending) {
                // AI\xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad = AI思考中
                snprintf(buf, sizeof(buf),
                    "AI\xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad... \xe6\x89\x8b\xe6\x95\xb0:%d",
                    s_move_count);
                lv_label_set_text(s_status_lbl, buf);
            } else {
                const char *turn = (s_player == 1) ?
                    "\xe9\xbb\x91\xe6\xa3\x8b" :   // 黑棋
                    "\xe7\x99\xbd\xe6\xa3\x8b";    // 白棋
                const char *ai = (s_mode == GO_PVE && s_player == 2) ? "(AI)" : "";
                snprintf(buf, sizeof(buf),
                    "%s%s \xe6\x89\x8b\xe6\x95\xb0:%d \xe6\x8f\x90\xe9\xbb\x91:%d \xe6\x8f\x90\xe7\x99\xbd:%d%s",
                    turn, ai, s_move_count, s_capt_w, s_capt_b,
                    (s_pass_count > 0) ? " \xe8\x99\x9a\xe6\x89\x8b!" : "");
                lv_label_set_text(s_status_lbl, buf);
            }
        } else {
            // 终局
            const char *winner = (s_score_b > s_score_w) ?
                "\xe9\xbb\x91\xe8\x83\x9c" :   // 黑胜
                "\xe7\x99\xbd\xe8\x83\x9c";    // 白胜
            snprintf(buf, sizeof(buf),
                "\xe7\xbb\x88\xe5\xb1\x80 \xe9\xbb\x91:%d \xe7\x99\xbd:%d(+%d) %s",
                s_score_b, s_score_w, GO_KOMI, winner);
            lv_label_set_text(s_status_lbl, buf);
        }
    }

    // 面板切换
    if (s_menu_panel) {
        if (s_state == GO_MENU) lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_game_panel) {
        if (s_state != GO_MENU) lv_obj_clear_flag(s_game_panel, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_game_panel, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_state == GO_PLAYING || s_state == GO_GAMEOVER) {
        render_board();
    }
}

static void timer_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    go_update();
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
    s_state = GO_MENU;
}

// ==================== 页面创建 ====================

extern "C" void game_go_show(void) {
    board_clear();
    s_state = GO_MENU;

    // PSRAM 画布
    if (!s_buf) {
        s_buf = (lv_color_t *)heap_caps_malloc(
            GO_CANVAS_W * GO_CANVAS_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    if (s_buf) memset(s_buf, 0, GO_CANVAS_W * GO_CANVAS_H * sizeof(lv_color_t));

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, GO_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, screen_unload_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    // 状态标签 (靠上, 避免与棋盘画布重叠)
    s_status_lbl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_status_lbl, CN_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_lbl, GO_TEXT, LV_PART_MAIN);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 6);

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
    lv_obj_set_style_text_color(diff_title, GO_TEXT, 0);
    lv_label_set_text(diff_title, "\xe9\x9a\xbe\xe5\xba\xa6");  // 难度
    lv_obj_align(diff_title, LV_ALIGN_TOP_MID, 0, y);
    y += 30;

    const char *diff_names[] = {
        "\xe7\xae\x80\xe5\x8d\x95",      // 简单
        "\xe5\x9b\xb0\xe9\x9a\xbe",      // 困难
        "\xe8\xb6\x85\xe7\xba\xa7\xe5\xa4\x8d\xe6\x9d\x82"  // 超级复杂
    };
    for (int i = 0; i < 3; i++) {
        lv_color_t c = (s_diff == i) ? GO_INFO : GO_DIM;
        s_diff_btns[i] = make_btn(s_menu_panel, 180, 40, diff_names[i], c, diff_cb, (void *)(intptr_t)i);
        lv_obj_align(s_diff_btns[i], LV_ALIGN_TOP_MID, 0, y);
        y += 48;
    }

    y += 10;

    // 模式选择
    lv_obj_t *mode_title = lv_label_create(s_menu_panel);
    lv_obj_set_style_text_font(mode_title, CN_FONT, 0);
    lv_obj_set_style_text_color(mode_title, GO_TEXT, 0);
    lv_label_set_text(mode_title, "\xe6\xa8\xa1\xe5\xbc\x8f");  // 模式
    lv_obj_align(mode_title, LV_ALIGN_TOP_MID, 0, y);
    y += 30;

    const char *mode_names[] = {
        "\xe4\xba\xba\xe6\x9c\xba\xe5\xaf\xb9\xe6\x88\x98",  // 人机对战
        "\xe5\x8f\x8c\xe4\xba\xba\xe5\xaf\xb9\xe6\x88\x98"    // 双人对战
    };
    for (int i = 0; i < 2; i++) {
        lv_color_t c = (s_mode == i) ? GO_WARN : GO_DIM;
        s_mode_btns[i] = make_btn(s_menu_panel, 180, 40, mode_names[i], c, mode_cb, (void *)(intptr_t)i);
        lv_obj_align(s_mode_btns[i], LV_ALIGN_TOP_MID, 0, y);
        y += 48;
    }

    y += 10;

    // 开始游戏按钮
    lv_obj_t *start_btn = make_btn(s_menu_panel, 180, 44,
        "\xe5\xbc\x80\xe5\xa7\x8b\xe6\xb8\xb8\xe6\x88\x8f",  // 开始游戏
        GO_SUCCESS, start_cb, NULL);
    lv_obj_align(start_btn, LV_ALIGN_TOP_MID, 0, y);

    // ===== 游戏面板 =====
    s_game_panel = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_game_panel);
    lv_obj_set_size(s_game_panel, GAME_W, GAME_H - UI_NAVBAR_H);
    lv_obj_align(s_game_panel, LV_ALIGN_TOP_LEFT, 0, UI_NAVBAR_H);
    lv_obj_set_style_bg_opa(s_game_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_game_panel, 0, 0);
    lv_obj_set_style_border_width(s_game_panel, 0, 0);
    lv_obj_clear_flag(s_game_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_game_panel, LV_OBJ_FLAG_HIDDEN);

    // 棋盘画布 (下移避免与顶部状态文字重叠, 棋盘水平居中)
    if (s_buf) {
        s_canvas = lv_canvas_create(s_game_panel);
        lv_canvas_set_buffer(s_canvas, s_buf, GO_CANVAS_W, GO_CANVAS_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(s_canvas, LV_ALIGN_TOP_MID, 0, 30);
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_canvas, canvas_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // 底部按钮（4 个：新局、悔棋、虚手、菜单）
    int btn_y = 30 + GO_CANVAS_H + 8;
    lv_obj_t *new_btn = make_btn(s_game_panel, 70, 32,
        "\xe6\x96\xb0\xe5\xb1\x80", GO_INFO, new_game_cb, NULL);  // 新局
    lv_obj_align(new_btn, LV_ALIGN_TOP_MID, -117, btn_y);

    lv_obj_t *undo_btn = make_btn(s_game_panel, 70, 32,
        "\xe6\x82\x94\xe6\xa3\x8b", GO_WARN, undo_cb, NULL);  // 悔棋
    lv_obj_align(undo_btn, LV_ALIGN_TOP_MID, -39, btn_y);

    lv_obj_t *pass_btn = make_btn(s_game_panel, 70, 32,
        "\xe8\x99\x9a\xe6\x89\x8b", GO_SUCCESS, pass_cb, NULL);  // 虚手
    lv_obj_align(pass_btn, LV_ALIGN_TOP_MID, 39, btn_y);

    lv_obj_t *menu_btn = make_btn(s_game_panel, 70, 32,
        "\xe8\x8f\x9c\xe5\x8d\x95", GO_DANGER, back_menu_cb, NULL);  // 菜单
    lv_obj_align(menu_btn, LV_ALIGN_TOP_MID, 117, btn_y);

    s_timer = lv_timer_create(timer_cb, GO_REFRESH, NULL);
    ui_navbar_push_screen_named(s_screen, true, "围棋");
    ui_refresh();
}

// ==================== 主循环更新 ====================

void go_update(void) {
    if (!s_screen || lv_screen_active() != s_screen) return;
    if (s_state != GO_PLAYING) return;
    if (!s_ai_pending) return;

    // AI 走棋（先保存历史用于悔棋）
    hist_push();
    bool moved = ai_move();
    if (moved) {
        s_move_count++;
        s_pass_count = 0;
    } else {
        // AI 虚手
        s_pass_count++;
        s_last_x = -1; s_last_y = -1;
    }

    if (s_pass_count >= 2) {
        calculate_score();
        s_state = GO_GAMEOVER;
    } else if (s_move_count >= GO_SIZE * GO_SIZE) {
        calculate_score();
        s_state = GO_GAMEOVER;
    } else {
        s_player = 3 - s_player;
    }
    s_ai_pending = false;
    ui_refresh();
}
