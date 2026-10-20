/*
 * paoku_game.c - 竖版跑酷游戏（裸渲染，不依赖 LVGL 控件）
 *
 * 技术要点：
 *  - 通过 esp_lvgl_adapter 的 dummy draw 模式接管/交还 MIPI 屏幕；
 *  - 游戏任务绑定 APP_CPU，帧率约 40-50FPS；
 *  - 贴图从 SD 卡 /sdcard/tietu/paoku/ 读取 PNG，解码后存放 PSRAM；
 *  - 三条纵向跑道，触摸左/右半屏切道，中上区域跳跃；
 *  - 三张 262x928 分层背景视差滚动；
 *  - 玩家奔跑/跳跃/死亡动画帧切换；
 *  - 退出时释放全部 PSRAM，恢复 LVGL 桌面。
 */
#include "paoku_game.h"
#include "app_display.h"
#include "bsp_board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* ---------- 屏幕与游戏参数 ---------- */
#define SCREEN_WIDTH   MIPI_DSI_LCD_H_RES   /* 262 */
#define SCREEN_HEIGHT  MIPI_DSI_LCD_V_RES   /* 928 */
#define TOP_HEIGHT     120                  /* 顶部 UI 区域高度 */
#define ROAD_TOP       TOP_HEIGHT           /* 跑道起点 */
#define LANES          3                    /* 跑道数量 */
#define LANE_W         (SCREEN_WIDTH / LANES)
#define MAX_SPRITES    12                   /* 同时活跃精灵上限 */
#define MAX_ASSETS     24                   /* 贴图资源上限 */
#define FRAME_MS       24                   /* 约 41.6 FPS */
#define SWIPE_THRESHOLD 16                  /* 滑动一格的手势位移阈值（像素） */
#define HORIZON_Y      380                  /* 伪3D 透视汇聚线（地平线） */
#define PLAYER_W       48                   /* 兜底玩家宽（无素材时） */
#define PLAYER_H       80                   /* 兜底玩家高 */
#define PLAYER_BASE_Y  (SCREEN_HEIGHT - 150)
#define JUMP_VY0       (-28)                /* 起跳初速度（像素/帧） */
#define GRAVITY        2                    /* 重力加速度 */
#define RUN_FRAME_TICK 4                    /* 每 N 帧切换奔跑帧 */
#define JUMP_FRAME_TICK 4
#define RGB565(r,g,b)  (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))

#include "lodepng.h"

static const char *TAG = "paoku";

/* ---------- 游戏状态机 ---------- */
typedef enum {
    GAME_IDLE = 0,
    GAME_RUN,
    GAME_OVER,
} game_state_t;

/* ---------- 贴图分类 ---------- */
typedef enum {
    ASSET_PLAYER_RUN = 0,   /* 奔跑帧 run*.png */
    ASSET_PLAYER_JUMP,      /* 跳跃帧 jump*.png */
    ASSET_PLAYER_DEAD,      /* 死亡帧 dead*.png */
    ASSET_OBSTACLE,         /* 障碍物 obs*.png */
    ASSET_COLLECT,          /* 金币 coin*.png */
    ASSET_BG_FAR,           /* 远景 bg_far.png */
    ASSET_BG_MID,           /* 中景 bg_mid.png */
    ASSET_BG_NEAR,          /* 近景 bg_near.png */
    ASSET_TYPE_MAX,
} asset_type_t;

/* ---------- 数据结构 ---------- */
typedef struct {
    uint16_t *pixels;       /* RGB565 位图（PSRAM），键色 0x0001 表示透明 */
    uint16_t width;
    uint16_t height;
    asset_type_t type;
} sprite_asset_t;

typedef struct {
    const sprite_asset_t *asset;
    int x;
    int y;
    int lane;
    bool active;
    bool coin;
} world_sprite_t;

/* ---------- 全局状态 ---------- */
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_exit_done;
static lv_display_t *s_display;
static bool s_use_adapter;   /* true=adapter dummy draw 路径；false=lvgl_port 暂停 + panel 直写 */
static volatile bool s_exit_requested;
static volatile bool s_running;
static game_state_t s_state = GAME_IDLE;

static uint16_t *s_frame;                   /* 全屏帧缓冲（PSRAM） */
static sprite_asset_t s_assets[MAX_ASSETS];
static int s_asset_count;
static world_sprite_t s_world[MAX_SPRITES];

static int s_player_lane = 1;               /* 目标跑道 */
static int s_player_x;                      /* 玩家当前像素 X（平滑插值） */
static bool s_jumping;                      /* 是否处于跳跃 */
static int s_jump_y;                        /* 玩家 Y 偏移（负值向上） */
static int s_jump_vy;                       /* 跳跃垂直速度 */
static int s_anim_tick;                     /* 动画帧计数 */
static int s_run_idx;                       /* 当前奔跑帧索引 */
static int s_jump_idx;                      /* 当前跳跃帧索引 */

static int s_score;
static int s_best;
static int s_speed;
static int s_bg_scroll;                     /* 背景滚动累计值 */
static uint32_t s_rng;
static bool s_swipe_active;                 /* 触摸按住状态 */
static int s_swipe_start_x = -1;            /* 手势起点 X */
static int s_swipe_start_y = -1;            /* 手势起点 Y */

/* ---------- PSRAM 分配 ---------- */
static void *psram_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* ---------- PNG 解码与资源加载 ---------- */
static bool has_png(const char *name)
{
    size_t n = strlen(name);
    return n >= 4 && strcasecmp(name + n - 4, ".png") == 0;
}

static void free_asset(sprite_asset_t *asset)
{
    if (asset->pixels) free(asset->pixels);
    memset(asset, 0, sizeof(*asset));
}

static void free_assets(void)
{
    for (int i = 0; i < s_asset_count; i++) free_asset(&s_assets[i]);
    s_asset_count = 0;
}

/* 从指定路径解码一张 PNG 并转换为 RGB565 存 PSRAM */
static bool load_png_asset(const char *path, asset_type_t type)
{
    if (s_asset_count >= MAX_ASSETS) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (file_size <= 0 || file_size > 1024 * 1024) {
        fclose(fp);
        return false;
    }
    unsigned char *file_data = psram_alloc((size_t)file_size);
    if (!file_data || fread(file_data, 1, (size_t)file_size, fp) != (size_t)file_size) {
        fclose(fp);
        free(file_data);
        return false;
    }
    fclose(fp);

    unsigned char *rgba = NULL;
    unsigned width = 0, height = 0;
    /*
     * LVGL 内置 lodepng 为裁剪版（LodePNGState 无自定义分配器接口），
     * 使用 decode32：大图（>16KB）解码缓冲由 SPIRAM_USE_MALLOC 自动走 PSRAM，
     * 小图走 internal（单张峰值有限，解码后立即释放）。
     */
    unsigned err = lodepng_decode32(&rgba, &width, &height,
                                    file_data, (size_t)file_size);
    free(file_data);
    if (err || !rgba || width == 0 || height == 0 || width > 262 || height > 928) {
        free(rgba);
        return false;
    }

    uint16_t *pixels = psram_alloc((size_t)width * height * sizeof(uint16_t));
    if (!pixels) {
        free(rgba);
        return false;
    }
    for (unsigned i = 0; i < width * height; i++) {
        unsigned char r = rgba[i * 4 + 0];
        unsigned char g = rgba[i * 4 + 1];
        unsigned char b = rgba[i * 4 + 2];
        unsigned char a = rgba[i * 4 + 3];
        pixels[i] = a < 16 ? 0x0001 : RGB565(r, g, b);   /* 半透明以下视为透明键色 */
    }
    free(rgba);
    s_assets[s_asset_count++] = (sprite_asset_t){
        .pixels = pixels, .width = (uint16_t)width, .height = (uint16_t)height, .type = type
    };
    return true;
}

/* 文件名是否以指定前缀开头（大小写不敏感） */
static bool name_has_prefix(const char *name, const char *prefix)
{
    size_t n = strlen(prefix);
    return strlen(name) > n && strncasecmp(name, prefix, n) == 0;
}

/* 加载目录下符合前缀的文件到对应分类 */
static void load_asset_dir_prefix(const char *dir_path, const char *prefix,
                                  asset_type_t type, int limit)
{
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    struct dirent *ent;
    int loaded = 0;
    while ((ent = readdir(dir)) != NULL && loaded < limit && s_asset_count < MAX_ASSETS) {
        if (!has_png(ent->d_name) || !name_has_prefix(ent->d_name, prefix)) continue;
        char path[192];
        snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
        if (load_png_asset(path, type)) loaded++;
    }
    closedir(dir);
}

static void load_assets(void)
{
    free_assets();
    /* 玩家：奔跑/跳跃/死亡动画帧 */
    load_asset_dir_prefix("/sdcard/tietu/paoku/player", "run", ASSET_PLAYER_RUN, 8);
    load_asset_dir_prefix("/sdcard/tietu/paoku/player", "jump", ASSET_PLAYER_JUMP, 4);
    load_asset_dir_prefix("/sdcard/tietu/paoku/player", "dead", ASSET_PLAYER_DEAD, 2);
    /* 障碍物 / 金币 */
    load_asset_dir_prefix("/sdcard/tietu/paoku/obstacle", "obs", ASSET_OBSTACLE, 8);
    load_asset_dir_prefix("/sdcard/tietu/paoku/collect", "coin", ASSET_COLLECT, 4);
    /* 视差背景：远/中/近三层 */
    load_asset_dir_prefix("/sdcard/tietu/paoku/bg", "bg_far", ASSET_BG_FAR, 1);
    load_asset_dir_prefix("/sdcard/tietu/paoku/bg", "bg_mid", ASSET_BG_MID, 1);
    load_asset_dir_prefix("/sdcard/tietu/paoku/bg", "bg_near", ASSET_BG_NEAR, 1);
    ESP_LOGI(TAG, "资源加载完成: %d 个 PNG", s_asset_count);
    ESP_LOGI(TAG, "加载后内存: internal=%uKB psram=%uKB",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    if (s_asset_count == 0) {
        ESP_LOGW(TAG, "未加载到任何贴图，请确认 SD 卡 /sdcard/tietu/paoku 素材就位");
    }
}

/* 收集指定类型的所有资源 */
static int find_assets(asset_type_t type, const sprite_asset_t **out, int max)
{
    int n = 0;
    for (int i = 0; i < s_asset_count && n < max; i++) {
        if (s_assets[i].type == type) out[n++] = &s_assets[i];
    }
    return n;
}

/* ---------- 裸渲染：矩形填充 / 透明 blit ---------- */
static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_frame) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > SCREEN_WIDTH ? SCREEN_WIDTH : x + w;
    int y1 = y + h > SCREEN_HEIGHT ? SCREEN_HEIGHT : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *dst = s_frame + yy * SCREEN_WIDTH + x0;
        for (int xx = x0; xx < x1; xx++) *dst++ = color;
    }
}

/* 固定尺寸、无缩放的透明 blit（键色 0x0001 跳过） */
static void blit_sprite(const sprite_asset_t *asset, int x, int y)
{
    if (!asset || !asset->pixels || !s_frame) return;
    for (int sy = 0; sy < asset->height; sy++) {
        int dy = y + sy;
        if (dy < 0 || dy >= SCREEN_HEIGHT) continue;
        for (int sx = 0; sx < asset->width; sx++) {
            int dx = x + sx;
            if (dx < 0 || dx >= SCREEN_WIDTH) continue;
            uint16_t pixel = asset->pixels[sy * asset->width + sx];
            if (pixel != 0x0001) s_frame[dy * SCREEN_WIDTH + dx] = pixel;
        }
    }
}

/* ---------- 视差背景：三层不同滚动速度 ---------- */
/* scale 越大滚动越快；每帧将背景内容向下推移，营造向前奔跑感 */
static void draw_bg_layer(const sprite_asset_t *bg, int scale)
{
    if (!bg || !bg->pixels || bg->height <= 0) return;
    /* 背景图宽度必须等于屏幕宽，否则行复制会错位/越界 */
    if (bg->width != SCREEN_WIDTH) {
        fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, RGB565(20, 26, 40));
        return;
    }
    int h = bg->height;
    int offset = ((s_bg_scroll * scale) / 10) % h;   /* 内容下移的行数 */
    int seg1 = h - offset;                            /* 第一段：src[offset..h-1] -> dst[0..] */
    if (seg1 > SCREEN_HEIGHT) seg1 = SCREEN_HEIGHT;
    memcpy(s_frame, bg->pixels + (size_t)offset * SCREEN_WIDTH,
           (size_t)seg1 * SCREEN_WIDTH * sizeof(uint16_t));
    if (seg1 < SCREEN_HEIGHT) {
        memcpy(s_frame + (size_t)seg1 * SCREEN_WIDTH, bg->pixels,
               (size_t)(SCREEN_HEIGHT - seg1) * SCREEN_WIDTH * sizeof(uint16_t));
    }
}

/* 画线（Bresenham），用于伪3D 透视车道 */
static void draw_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (1) {
        if (x0 >= 0 && x0 < SCREEN_WIDTH && y0 >= 0 && y0 < SCREEN_HEIGHT) {
            s_frame[y0 * SCREEN_WIDTH + x0] = color;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

/*
 * 伪3D 透视道路（地铁跑酷式纵深感）：
 * 三条跑道从屏幕底部（近端）向地平线（远端）汇聚，车道线呈放射状，
 * 配合向下滚动的横条纹营造向前奔跑的速度感。
 */
static void draw_perspective_road(void)
{
    int horizon = HORIZON_Y;

    /* 路面底色：地平线以下全宽 */
    fill_rect(0, horizon, SCREEN_WIDTH, SCREEN_HEIGHT - horizon, RGB565(34, 38, 48));

    /* 两侧护栏（近端贴边，远端向中间汇聚） */
    draw_line(0, SCREEN_HEIGHT, 108, horizon, RGB565(120, 130, 150));
    draw_line(2, SCREEN_HEIGHT, 110, horizon, RGB565(90, 100, 120));
    draw_line(SCREEN_WIDTH - 1, SCREEN_HEIGHT, 154, horizon, RGB565(120, 130, 150));
    draw_line(SCREEN_WIDTH - 3, SCREEN_HEIGHT, 152, horizon, RGB565(90, 100, 120));

    /* 两条车道分隔线：近端约 87/175，远端汇聚到屏幕中心 131 */
    draw_line(87, SCREEN_HEIGHT, 131, horizon, RGB565(200, 200, 205));
    draw_line(89, SCREEN_HEIGHT, 132, horizon, RGB565(150, 150, 155));
    draw_line(175, SCREEN_HEIGHT, 131, horizon, RGB565(200, 200, 205));
    draw_line(173, SCREEN_HEIGHT, 130, horizon, RGB565(150, 150, 155));

    /* 速度感横条纹：整体向下滚动循环 */
    int offset = (s_bg_scroll * 2) % 96;
    for (int y = horizon + offset; y < SCREEN_HEIGHT; y += 96) {
        for (int k = 0; k < LANES; k++) {
            int cx = 43 + k * 87;   /* 三条车道中心 */
            if (cx >= 0 && cx < SCREEN_WIDTH) {
                fill_rect(cx - 10, y, 20, 3, RGB565(92, 98, 112));
            }
        }
    }
    offset = (s_bg_scroll * 2 + 48) % 160;
    for (int y = horizon + offset; y < SCREEN_HEIGHT; y += 160) {
        for (int k = 0; k < LANES; k++) {
            int cx = 43 + k * 87;
            if (cx >= 0 && cx < SCREEN_WIDTH) {
                fill_rect(cx - 14, y, 28, 4, RGB565(60, 66, 78));
            }
        }
    }
}

static void draw_background(void)
{
    const sprite_asset_t *bg[2] = {NULL, NULL};
    find_assets(ASSET_BG_FAR, &bg[0], 1);
    find_assets(ASSET_BG_MID, &bg[1], 1);

    if (bg[0]) {
        draw_bg_layer(bg[0], 3);          /* 远景：慢速滚动 */
    } else {
        fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, RGB565(14, 20, 32));
    }
    if (bg[1]) draw_bg_layer(bg[1], 6);   /* 中景：中速滚动 */
    /* 近景：伪3D 透视道路（程序化绘制，覆盖地平线以下） */
    draw_perspective_road();
}

/* ---------- 顶部 UI（分数 / 最高分 / 退出按钮 / 速度条） ---------- */
static void draw_ui(void)
{
    fill_rect(0, 0, SCREEN_WIDTH, TOP_HEIGHT, RGB565(11, 13, 22));
    fill_rect(8, 12, 58, 42, RGB565(185, 55, 66));       /* 分数色块 */
    fill_rect(196, 12, 58, 42, RGB565(50, 130, 190));    /* 最高分色块 */
    fill_rect(8, 64, 246, 2, RGB565(90, 95, 110));
    fill_rect(12, 78, (s_score % 220) + 1, 8, RGB565(245, 185, 55));
    fill_rect(12, 94, (s_best > 0 ? (s_best % 220) + 1 : 1), 5, RGB565(90, 190, 120));
    fill_rect(218, 72, 30, 32, RGB565(190, 60, 70));     /* 退出按钮 */
}

/* ---------- 游戏结束界面 ---------- */
static void draw_game_over(void)
{
    int bx = 31, by = 380, bw = 200, bh = 150;
    fill_rect(bx - 3, by - 3, bw + 6, bh + 6, RGB565(220, 60, 70));
    fill_rect(bx, by, bw, bh, RGB565(20, 22, 34));
    fill_rect(bx + 20, by + 18, bw - 40, 14, RGB565(245, 185, 55));
    fill_rect(bx + 20, by + 56, (s_score > 0 ? (s_score % (bw - 40)) + 1 : 1), 10, RGB565(255, 255, 255));
    fill_rect(bx + 20, by + 92, bw - 40, 8, RGB565(90, 190, 120));
    fill_rect(bx + 50, by + 118, bw - 100, 14, RGB565(60, 120, 210));  /* 重新开始提示区 */
}

/* ---------- 玩家绘制（按状态切换动画帧） ---------- */
static const sprite_asset_t *player_anim_frame(bool *jumping)
{
    const sprite_asset_t *list[8];
    const sprite_asset_t *frame = NULL;
    *jumping = s_jumping;

    if (s_state == GAME_OVER) {
        int n = find_assets(ASSET_PLAYER_DEAD, list, 8);
        if (n > 0) return list[s_anim_tick / 8 % n];
        frame = NULL;
    } else if (s_jumping) {
        int n = find_assets(ASSET_PLAYER_JUMP, list, 8);
        if (n > 0) return list[s_jump_idx % n];
        frame = NULL;
    } else {
        int n = find_assets(ASSET_PLAYER_RUN, list, 8);
        if (n > 0) return list[s_run_idx % n];
        frame = NULL;
    }
    if (!frame) {
        int n = find_assets(ASSET_PLAYER_RUN, list, 8);
        if (n > 0) return list[0];
    }
    return NULL;
}

static void draw_world(void)
{
    bool jumping = false;
    const sprite_asset_t *player = player_anim_frame(&jumping);
    int pw = player ? player->width : PLAYER_W;
    int ph = player ? player->height : PLAYER_H;

    /* 脚下阴影：固定贴地，跳跃时仍在地面（伪3D 接地感） */
    fill_rect(s_player_x + pw / 2 - 11, PLAYER_BASE_Y + ph - 3, 22, 7, RGB565(10, 12, 18));

    if (player) {
        blit_sprite(player, s_player_x, PLAYER_BASE_Y + s_jump_y);
    } else {
        /* 无素材兜底色块 */
        fill_rect(s_player_x, PLAYER_BASE_Y + s_jump_y, PLAYER_W, PLAYER_H,
                  s_jumping ? RGB565(240, 200, 60) : RGB565(80, 210, 120));
    }

    /* 金币帧循环动画 */
    const sprite_asset_t *coin[4];
    int coin_n = find_assets(ASSET_COLLECT, coin, 4);

    for (int i = 0; i < MAX_SPRITES; i++) {
        if (!s_world[i].active || !s_world[i].asset) continue;
        const sprite_asset_t *a = s_world[i].asset;
        if (s_world[i].coin && coin_n > 1) {
            a = coin[(s_anim_tick / 6) % coin_n];
        }
        int x = s_world[i].lane * LANE_W + (LANE_W - a->width) / 2;
        blit_sprite(a, x, s_world[i].y);
    }
}

/* ---------- 世界生成 / 更新 / 碰撞 ---------- */
static void spawn_world(void)
{
    for (int i = 0; i < MAX_SPRITES; i++) {
        if (s_world[i].active) continue;
        bool coin = (s_rng++ % 5) == 0;
        const sprite_asset_t *list[8];
        const sprite_asset_t *asset = NULL;
        int n = find_assets(coin ? ASSET_COLLECT : ASSET_OBSTACLE, list, 8);
        if (n > 0) asset = list[s_rng++ % n];
        if (!asset) return;
        s_world[i] = (world_sprite_t){
            .asset = asset,
            .y = ROAD_TOP - 30 - (int)(s_rng % 120),
            .lane = (int)(s_rng % LANES),
            .active = true,
            .coin = coin,
        };
        return;
    }
}

static bool overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static void update_world(void)
{
    /* 背景滚动累计（视差滚动量） */
    s_bg_scroll += s_speed;

    /* 动画帧推进 */
    s_anim_tick++;
    const sprite_asset_t *run_list[8];
    int run_n = find_assets(ASSET_PLAYER_RUN, run_list, 8);
    if (run_n > 0 && (s_anim_tick % RUN_FRAME_TICK) == 0) s_run_idx = (s_run_idx + 1) % run_n;
    const sprite_asset_t *jump_list[8];
    int jump_n = find_assets(ASSET_PLAYER_JUMP, jump_list, 8);
    if (jump_n > 0 && (s_anim_tick % JUMP_FRAME_TICK) == 0) s_jump_idx = (s_jump_idx + 1) % jump_n;

    /* 跳跃物理：上升 -> 落地 */
    if (s_jumping) {
        s_jump_vy += GRAVITY;
        s_jump_y += s_jump_vy;
        if (s_jump_y >= 0) {
            s_jump_y = 0;
            s_jumping = false;
        }
    }

    /* 平滑横向切换跑道 */
    const sprite_asset_t *plist[8];
    int pn = find_assets(ASSET_PLAYER_RUN, plist, 8);
    int pw = (pn > 0) ? plist[0]->width : PLAYER_W;
    int ph = (pn > 0) ? plist[0]->height : PLAYER_H;
    int target_x = s_player_lane * LANE_W + (LANE_W - pw) / 2;
    s_player_x += (target_x - s_player_x) * 3 / 10;
    if (s_player_x < 0) s_player_x = 0;
    if (s_player_x + pw > SCREEN_WIDTH) s_player_x = SCREEN_WIDTH - pw;

    /* 玩家碰撞盒（考虑跳跃偏移） */
    int player_y = PLAYER_BASE_Y + s_jump_y;

    for (int i = 0; i < MAX_SPRITES; i++) {
        if (!s_world[i].active) continue;
        s_world[i].y += s_speed;
        int ox = s_world[i].lane * LANE_W + (LANE_W - s_world[i].asset->width) / 2;
        int ow = s_world[i].asset->width;
        int oh = s_world[i].asset->height;

        if (overlap(s_player_x, player_y, pw, ph, ox, s_world[i].y, ow, oh)) {
            if (s_world[i].coin) {
                s_score += 10;
                s_world[i].active = false;
            } else {
                s_state = GAME_OVER;
                if (s_score > s_best) s_best = s_score;
                break;   /* 死亡后不再处理其余精灵 */
            }
        }
        if (s_world[i].y > SCREEN_HEIGHT) {
            s_world[i].active = false;
            if (!s_world[i].coin) s_score++;   /* 每通过一个障碍物加分 */
        }
    }

    /* 难度曲线：分数越高滚动越快、生成越频繁 */
    if (s_score > 0 && (s_score % 20) == 0) {
        int ns = 3 + s_score / 80;
        if (ns > 12) ns = 12;
        s_speed = ns;
    }
    if ((s_rng++ % (18 - (s_score / 200 > 8 ? 8 : s_score / 200))) == 0) spawn_world();
}

/* ---------- 触摸输入（底层直接读取，不经 LVGL） ---------- */
static void do_jump(void)
{
    if (s_jumping) return;
    s_jumping = true;
    s_jump_y = 0;
    s_jump_vy = JUMP_VY0;
}

static void restart_game(void)
{
    memset(s_world, 0, sizeof(s_world));
    s_score = 0;
    s_speed = 3;
    s_jumping = false;
    s_jump_y = 0;
    s_jump_vy = 0;
    s_anim_tick = 0;
    s_run_idx = 0;
    s_jump_idx = 0;
    s_player_lane = 1;
    s_state = GAME_RUN;
}

/*
 * 手势规则：
 *  - 按下瞬间：顶部右侧=退出；中上区域=跳跃；游戏结束底部=重新开始；
 *  - 按住滑动：左滑/右滑只移动一格跑道（左滑->左移一道，右滑->右移一道），
 *    位移超过阈值后重置起点，支持连续多格滑动。
 */
static void read_touch(void)
{
    esp_lcd_touch_handle_t touch = app_display_touch();
    if (!touch) return;
    if (esp_lcd_touch_read_data(touch) != ESP_OK) return;
    esp_lcd_touch_point_data_t pt = {0};
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(touch, &pt, &count, 1) != ESP_OK) return;

    if (count == 0) {
        /* 手指已抬起 */
        s_swipe_active = false;
        s_swipe_start_x = -1;
        s_swipe_start_y = -1;
        return;
    }
    uint16_t x = pt.x;
    uint16_t y = pt.y;

    if (!s_swipe_active) {
        /* ---- 新按下：处理点击语义 ---- */
        s_swipe_active = true;
        s_swipe_start_x = x;
        s_swipe_start_y = y;

        if (y < TOP_HEIGHT && x > 205) {
            s_exit_requested = true;   /* 顶部右侧退出按钮 */
            return;
        }
        if (s_state == GAME_RUN && y < 360) {
            do_jump();                 /* 中上区域跳跃 */
            return;
        }
        if (s_state == GAME_OVER && y > 700) {
            restart_game();            /* 游戏结束重新开始 */
            return;
        }
        return;
    }

    /* ---- 按住滑动：一格一格切道 ---- */
    if (s_state != GAME_RUN) return;
    int dx = (int)x - s_swipe_start_x;
    int dy = (int)y - s_swipe_start_y;
    if (dx > SWIPE_THRESHOLD && dx > dy && dx > -dy) {
        /* 右滑：右移一道 */
        if (s_player_lane < LANES - 1) s_player_lane++;
        s_swipe_start_x = x;   /* 重置起点，支持连续滑动 */
        s_swipe_start_y = y;
    } else if (dx < -SWIPE_THRESHOLD && -dx > dy && -dx > -dy) {
        /* 左滑：左移一道 */
        if (s_player_lane > 0) s_player_lane--;
        s_swipe_start_x = x;
        s_swipe_start_y = y;
    }
}

/* ---------- 显示接管：adapter dummy draw 优先，失败回退 lvgl_port 暂停 ---------- */
/*
 * 说明：esp_lvgl_adapter 的 dummy draw 需要 adapter 自身初始化并注册显示管道，
 * 当前工程基于 esp_lvgl_port 体系，adapter 未初始化时会返回 ESP_ERR_INVALID_STATE，
 * 此时回退到 lvgl_port_stop()：仅冻结 LVGL 渲染（tick/timer），不销毁任何对象，
 * 游戏直接写 panel；退出时 lvgl_port_resume() 恢复桌面。
 */
static esp_err_t enter_dummy_draw(lv_display_t *disp)
{
    esp_err_t err = esp_lv_adapter_set_dummy_draw(disp, true);
    if (err == ESP_OK) {
        s_use_adapter = true;
        ESP_LOGI(TAG, "已进入 esp_lvgl_adapter dummy draw 模式");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "esp_lvgl_adapter dummy draw 不可用 (%s)，回退 lvgl_port 暂停渲染", esp_err_to_name(err));
    err = lvgl_port_stop();
    if (err == ESP_OK) {
        s_use_adapter = false;
        ESP_LOGI(TAG, "已暂停 LVGL 渲染，进入裸渲染模式");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "lvgl_port_stop 失败: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t exit_dummy_draw(void)
{
    if (s_use_adapter) {
        s_use_adapter = false;
        return esp_lv_adapter_set_dummy_draw(s_display, false);
    }
    return lvgl_port_resume();
}

static esp_err_t frame_flush(void)
{
    if (s_use_adapter) {
        return esp_lv_adapter_dummy_draw_blit(
            s_display, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, s_frame, true);
    }
    return esp_lcd_panel_draw_bitmap(app_display_panel(), 0, 0,
                                     SCREEN_WIDTH, SCREEN_HEIGHT, s_frame);
}

/* ---------- 渲染输出 ---------- */
static void render_frame(void)
{
    draw_background();
    draw_ui();
    draw_world();
    if (s_state == GAME_OVER) draw_game_over();

    if (s_display) {
        esp_err_t err = frame_flush();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "帧刷新失败: %s", esp_err_to_name(err));
            s_exit_requested = true;
        }
    }
}

/* ---------- 清理：释放全部 PSRAM，恢复 LVGL ---------- */
static void cleanup_game(void)
{
    free_assets();
    free(s_frame);
    s_frame = NULL;
    s_running = false;
    s_state = GAME_IDLE;
    s_task = NULL;
    if (s_display) {
        esp_err_t err = exit_dummy_draw();
        if (err != ESP_OK) ESP_LOGE(TAG, "恢复 LVGL 显示失败: %s", esp_err_to_name(err));
        s_display = NULL;
    }
    if (s_exit_done) xSemaphoreGive(s_exit_done);
    ESP_LOGI(TAG, "跑酷退出，LVGL 已恢复");
}

/* ---------- 游戏任务（绑定 APP_CPU） ---------- */
static void paoku_task(void *arg)
{
    lv_display_t *disp = (lv_display_t *)arg;

    /* 帧缓冲：全屏 PSRAM */
    s_frame = psram_alloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
    if (!s_frame) {
        ESP_LOGE(TAG, "无法分配 PSRAM 帧缓冲");
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* 加载贴图资源（解码走 PSRAM） */
    load_assets();

    /* 接管显示：adapter 优先，失败回退 lvgl_port 暂停（此时才脱离 LVGL 事件回调上下文） */
    if (enter_dummy_draw(disp) != ESP_OK) {
        free_assets();
        free(s_frame);
        s_frame = NULL;
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_display = disp;
    s_state = GAME_RUN;

    while (!s_exit_requested) {
        read_touch();
        if (s_state == GAME_RUN) update_world();
        render_frame();
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }

    cleanup_game();
    vTaskDelete(NULL);
}

/* ---------- 对外接口 ---------- */
/* 该函数在 LVGL 事件回调（LVGL 任务上下文）中被调用，因此只做：
 * 状态重置 + 创建游戏任务；显示接管和渲染切换放到游戏任务内部执行。 */
void paoku_game_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "跑酷已在运行，忽略重复启动");
        return;
    }
    lv_display_t *display = app_display_lvgl_display();
    esp_lcd_panel_handle_t panel = app_display_panel();
    ESP_LOGI(TAG, "请求启动跑酷: disp=%p panel=%p", (void *)display, (void *)panel);
    if (!display || !panel) {
        ESP_LOGE(TAG, "显示句柄无效，无法启动跑酷");
        return;
    }
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_exit_done) s_exit_done = xSemaphoreCreateBinary();
    if (!s_lock || !s_exit_done) {
        ESP_LOGE(TAG, "信号量创建失败");
        return;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "跑酷正在启动中，忽略本次请求");
        return;
    }

    /* 重置游戏状态 */
    s_display = NULL;
    s_use_adapter = false;
    s_exit_requested = false;
    s_running = true;
    s_state = GAME_IDLE;
    s_score = 0;
    s_speed = 3;
    s_bg_scroll = 0;
    s_player_lane = 1;
    s_player_x = LANE_W + (LANE_W - PLAYER_W) / 2;
    s_jumping = false;
    s_jump_y = 0;
    s_jump_vy = 0;
    s_anim_tick = 0;
    s_run_idx = 0;
    s_jump_idx = 0;
    s_rng = (uint32_t)esp_random();
    memset(s_world, 0, sizeof(s_world));

    if (xSemaphoreTake(s_exit_done, 0) == pdTRUE) {
        /* 清空上一次任务的完成信号，避免误判退出完成。 */
    }

    /*
     * 任务栈放 PSRAM（MALLOC_CAP_SPIRAM），不挤占 internal RAM。
     * internal 紧张会导致 SDMMC 读扇区分配 DMA 缓冲失败（err=0x101）。
     */
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        paoku_task, "paoku", 4096, display, 5, &s_task, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        s_running = false;
        s_task = NULL;
        ESP_LOGE(TAG, "跑酷任务创建失败 (%d)", ret);
    }
    xSemaphoreGive(s_lock);
}

void paoku_game_exit(void)
{
    if (!s_running) return;
    s_exit_requested = true;
    if (xTaskGetCurrentTaskHandle() != s_task && s_exit_done) {
        xSemaphoreTake(s_exit_done, pdMS_TO_TICKS(1000));
    }
}
