/**
 * @file ui_gesture.c
 * @brief 侧滑返回手势模块实现
 *
 * 在屏幕左侧边缘创建不可见触摸区域,
 * 当用户从左边缘向右滑动时触发返回回调 (类似iOS侧滑返回)。
 * 使用 LV_EVENT_PRESSING 实时检测滑动, 响应更灵敏。
 */
#include "ui_gesture.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"

static const char *TAG = "ui_gesture";

/* 左边缘检测宽度 - 增大检测区域提高灵敏度 */
#define EDGE_WIDTH       50

/* 触发返回的最小水平滑动距离(像素) */
#define SWIPE_THRESHOLD  30

/* 上下文: 记录按下位置 + 回调指针 */
typedef struct {
    lv_point_t press_point;
    bool pressed;
    bool triggered;
    void (*back_cb)(void);
} edge_swipe_ctx_t;

/* 存储所有手势上下文 */
#define MAX_EDGE_CONTEXTS 16
static edge_swipe_ctx_t *s_contexts[MAX_EDGE_CONTEXTS] = {NULL};
static int s_context_count = 0;

/* 按下事件: 记录触摸起始坐标 */
static void edge_pressed_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    edge_swipe_ctx_t *ctx = (edge_swipe_ctx_t *)lv_obj_get_user_data(target);
    if (!ctx) return;

    lv_indev_t *indev = lv_indev_get_act();
    if (indev) {
        lv_indev_get_point(indev, &ctx->press_point);
        ctx->pressed = true;
        ctx->triggered = false;
    }
}

/* 按住移动事件: 实时检测滑动距离 */
static void edge_pressing_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    edge_swipe_ctx_t *ctx = (edge_swipe_ctx_t *)lv_obj_get_user_data(target);
    if (!ctx || !ctx->pressed || ctx->triggered) return;

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t cur_point;
    lv_indev_get_point(indev, &cur_point);

    int32_t dx = cur_point.x - ctx->press_point.x;
    int32_t dy = cur_point.y - ctx->press_point.y;

    /* 条件: 水平向右滑动超过阈值, 且主要是水平移动 */
    if (dx > SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
        ctx->triggered = true;
        ESP_LOGI(TAG, "侧滑返回触发: dx=%ld dy=%ld", (long)dx, (long)dy);
        if (ctx->back_cb) {
            ctx->back_cb();
        }
    }
}

/* 释放事件: 重置状态 */
static void edge_released_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    edge_swipe_ctx_t *ctx = (edge_swipe_ctx_t *)lv_obj_get_user_data(target);
    if (!ctx) return;
    ctx->pressed = false;
    ctx->triggered = false;
}

void ui_edge_swipe_add(lv_obj_t *screen, void (*back_cb)(void))
{
    if (!screen || !back_cb) return;

    /* 分配上下文内存 */
    edge_swipe_ctx_t *ctx = heap_caps_malloc(sizeof(edge_swipe_ctx_t), MALLOC_CAP_SPIRAM);
    if (!ctx) {
        ESP_LOGW(TAG, "PSRAM分配失败，使用普通内存");
        ctx = malloc(sizeof(edge_swipe_ctx_t));
    }
    if (!ctx) {
        ESP_LOGE(TAG, "分配侧滑上下文内存失败");
        return;
    }
    memset(ctx, 0, sizeof(edge_swipe_ctx_t));
    ctx->back_cb = back_cb;

    /* 保存上下文引用 */
    if (s_context_count < MAX_EDGE_CONTEXTS) {
        s_contexts[s_context_count++] = ctx;
    }

    /* 创建左边缘不可见触摸区域 */
    lv_obj_t *edge = lv_obj_create(screen);
    lv_obj_remove_style_all(edge);
    lv_obj_set_size(edge, EDGE_WIDTH, LV_PCT(100));
    lv_obj_set_pos(edge, 0, 0);
    lv_obj_clear_flag(edge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(edge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(edge, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_bg_opa(edge, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(edge, 0, 0);
    lv_obj_set_style_border_width(edge, 0, 0);

    /* 将用户数据设置为上下文指针 */
    lv_obj_set_user_data(edge, ctx);

    /* 设置事件回调 */
    lv_obj_add_event_cb(edge, edge_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(edge, edge_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(edge, edge_released_cb, LV_EVENT_RELEASED, NULL);

    /* 将边缘区域移到最前面 */
    lv_obj_move_foreground(edge);

    ESP_LOGD(TAG, "侧滑手势已添加到屏幕");
}

/* 清理所有手势上下文 */
void ui_edge_swipe_remove_all(void)
{
    for (int i = 0; i < s_context_count; i++) {
        if (s_contexts[i]) {
            free(s_contexts[i]);
            s_contexts[i] = NULL;
        }
    }
    s_context_count = 0;
}
