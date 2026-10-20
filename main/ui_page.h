/*******************************************************************************
 * 统一页面切换框架 - LVGL 9.x (ESP-IDF C)
 *
 * 屏幕: 262x928 竖屏, MIPI-DSI
 *
 * 功能:
 *   - 页面栈管理 (push / replace / back), 栈内存放 PSRAM;
 *   - 统一页面切换接口 ui_page_push() / ui_page_back(), 内部自动选择过渡动画;
 *   - 过渡动画类型:
 *       · SLIDE_LEFT / SLIDE_RIGHT: 左右滑入滑出 (走 LVGL 内置
 *         lv_screen_load_anim + MOVE_*, 内部就是 lv_anim, 性能最优,
 *         且切换期间新旧屏同时渲染, 无撕裂);
 *       · FADE / OVER_*: 淡入淡出 / 覆盖滑入 (同样走内置动画);
 *       · ZOOM: 缩放+淡入复合过渡, 基于 lv_anim_timeline 时间轴
 *         (预置 scale/opa 样式 -> lv_screen_load 立即载入 -> 时间轴播放);
 *   - 手势滑动翻页: 在触摸 indev 上注册全局事件 (PRESSSED/RELEASED),
 *     右滑 = 返回上一页, 左滑 = 触发用户注册的"下一页"回调;
 *     (原理: LVGL9 的事件默认不会沿父级冒泡, 页面级手势必须挂到 indev 上)
 *   - 全部接口无阻塞, 动画由 LVGL 定时器驱动。
 *
 * 与 ui_navbar 的关系:
 *   ui_navbar 提供安卓三键导航栏 UI; ui_page 提供页面栈与过渡动画。
 *   建议二选一作为唯一导航来源, 或将 ui_navbar 的屏幕加载函数替换为
 *   ui_page_push()/ui_page_back(), 避免两套栈并存造成页面管理混乱。
 ******************************************************************************/
#ifndef UI_PAGE_H_INCLUDED
#define UI_PAGE_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

/* ========================================================================
 * 过渡动画类型
 * ======================================================================== */
typedef enum {
    UI_PAGE_TRANS_NONE = 0,      /* 无动画, 立即切换 */
    UI_PAGE_TRANS_SLIDE_LEFT,    /* 新页自右滑入, 旧页向左滑出 (push 前进) */
    UI_PAGE_TRANS_SLIDE_RIGHT,   /* 新页自左滑入, 旧页向右滑出 (back 返回) */
    UI_PAGE_TRANS_FADE,          /* 淡入淡出 */
    UI_PAGE_TRANS_ZOOM,          /* 缩放 + 淡入 (lv_anim_timeline 时间轴) */
    UI_PAGE_TRANS_OVER_LEFT,     /* 新页自右覆盖滑入, 旧页不动 */
    UI_PAGE_TRANS_OVER_RIGHT,    /* 新页自左覆盖滑入, 旧页不动 */
} ui_page_trans_t;

/* 默认过渡时长 (ms), 300ms 是 Android Material 推荐值 */
#define UI_PAGE_TRANS_TIME_DEFAULT  300

/* ========================================================================
 * 手势相关回调
 * ======================================================================== */

/**
 * @brief 手势左滑方向
 */
typedef enum {
    UI_PAGE_SWIPE_LEFT = 0,   /* 左滑 (通常表示"下一页") */
    UI_PAGE_SWIPE_RIGHT,      /* 右滑 (内部已处理为返回上一页) */
} ui_page_swipe_dir_t;

/**
 * @brief 页面手势允许回调: 返回 false 表示该页面不响应滑动翻页
 *
 * 典型用法: 列表页在列表未滚动到顶部时禁用返回手势。
 */
typedef bool (*ui_page_allow_cb_t)(lv_obj_t *scr, void *user_data);

/**
 * @brief 左滑回调: 返回 true 表示已处理 (例如切换到下一页)
 */
typedef bool (*ui_page_swipe_cb_t)(lv_obj_t *scr, ui_page_swipe_dir_t dir,
                                   void *user_data);

/* ========================================================================
 * 初始化 / 手势
 * ======================================================================== */

/**
 * @brief 初始化页面框架
 *
 * 在触摸 indev 创建完成后调用一次 (通常与 ui_navbar_init 同时)。
 * 内部注册全局触摸事件用于滑动翻页。
 */
void ui_page_init(void);

/**
 * @brief 开关手势滑动翻页
 *
 * 弹窗打开等场景建议临时关闭, 避免误触返回。
 */
void ui_page_set_gesture(bool enable);

/**
 * @brief 注册页面手势允许回调
 */
void ui_page_set_swipe_allow_cb(ui_page_allow_cb_t cb, void *user_data);

/**
 * @brief 注册左滑回调 (用于"下一页"等自定义操作)
 */
void ui_page_set_swipe_cb(ui_page_swipe_cb_t cb, void *user_data);

/* ========================================================================
 * 页面栈操作
 * ======================================================================== */

/**
 * @brief 压入新页面 (保存当前页到栈, 再切换)
 *
 * @param new_scr  新页面对象 (调用方创建并保持生命周期)
 * @param name     页面名称 (用于调试/最近应用列表, 可传 NULL)
 * @param trans    过渡动画类型
 * @param time     动画时长 (ms), 0 表示使用默认 300ms
 */
void ui_page_push(lv_obj_t *new_scr, const char *name,
                  ui_page_trans_t trans, uint32_t time);

/**
 * @brief 替换当前页 (不增加栈深度, 同级页面切换用)
 */
void ui_page_replace(lv_obj_t *new_scr, const char *name,
                     ui_page_trans_t trans, uint32_t time);

/**
 * @brief 返回上一页 (弹出栈顶并切换)
 *
 * 栈为空时: 若注册了 home 回调则调用, 否则仅打日志。
 */
void ui_page_back(ui_page_trans_t trans, uint32_t time);

/**
 * @brief 注册"栈空时返回"回调 (通常用于回到桌面)
 */
void ui_page_set_home_cb(void (*cb)(void));

/**
 * @brief 查询是否可以返回 (栈中是否有页面)
 */
bool ui_page_can_back(void);

/**
 * @brief 当前栈深度 (已压入的页面数量)
 */
int ui_page_depth(void);

/**
 * @brief 获取当前活动屏幕
 */
lv_obj_t *ui_page_current(void);

/* ========================================================================
 * 便捷封装 (使用默认时长与默认动画)
 * ======================================================================== */

/**
 * @brief 压入新页面, 使用 SLIDE_LEFT + 默认时长
 */
void ui_page_push_default(lv_obj_t *new_scr, const char *name);

/**
 * @brief 返回上一页, 使用 SLIDE_RIGHT + 默认时长
 */
void ui_page_back_default(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_PAGE_H_INCLUDED */
