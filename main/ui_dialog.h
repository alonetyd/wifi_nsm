/*******************************************************************************
 * 通用弹窗组件 - LVGL 9.x (ESP-IDF C)
 *
 * 屏幕: 262x928 竖屏, MIPI-DSI
 *
 * 功能:
 *   - 遮罩层 (mask) + 内容面板 (panel) 结构, 挂在 layer_top 之上, 不随屏幕切换消失;
 *   - 弹出动画: lv_anim_timeline 时间轴复合动画
 *       · 遮罩透明度 0 -> 200 (fade)
 *       · 面板缩放 0.8x -> 1.0x (overshoot 回弹)
 *       · 面板透明度 0 -> 255 + 上移 24px
 *   - 关闭动画: 同一时间轴反向回放 (lv_anim_timeline_set_reverse + start);
 *   - 关闭完成后自动释放时间轴, 可选自动销毁对话框对象;
 *   - 动画由 LVGL 定时器驱动, 无阻塞延时。
 *
 * 事件处理:
 *   - 点击遮罩关闭 (可配置);
 *   - 面板默认不可穿透点击, 下方控件在弹窗打开期间不会收到事件
 *     (因为 mask 是全屏可点击对象, 命中测试会拦截)。
 *
 * 内存:
 *   - ui_dialog_t 由本模块分配 (内部 malloc), 关闭并删除后释放;
 *   - 弹窗内容由调用方在 ui_dialog_get_content() 返回的容器中创建,
 *     随 mask 一起销毁。
 ******************************************************************************/
#ifndef UI_DIALOG_H_INCLUDED
#define UI_DIALOG_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

/* 弹窗句柄 (不透明类型) */
typedef struct ui_dialog_t ui_dialog_t;

/**
 * @brief 弹窗关闭完成回调 (仅在关闭动画播放完毕后触发)
 *
 * @param dlg       对话框句柄
 * @param user_data 注册回调时传入的用户数据
 */
typedef void (*ui_dialog_close_cb_t)(ui_dialog_t *dlg, void *user_data);

/* ========================================================================
 * 创建 / 销毁
 * ======================================================================== */

/**
 * @brief 创建弹窗 (对象树挂到 parent 下, parent 为 NULL 时挂到 layer_top)
 *
 * 创建后默认隐藏, 调用 ui_dialog_show() 显示并播放弹出动画。
 * 弹窗初始尺寸 240x200, 可通过 ui_dialog_set_size() 修改。
 *
 * @param parent 父对象 (通常传 NULL 使用 layer_top)
 * @return 弹窗句柄, 失败返回 NULL
 */
ui_dialog_t *ui_dialog_create(lv_obj_t *parent);

/**
 * @brief 立即销毁弹窗 (不播放关闭动画)
 *
 * 内部会删除时间轴与全部对象 (mask/panel/内容), 并释放句柄。
 * 若关闭动画正在进行中调用, 会直接终止动画并销毁。
 */
void ui_dialog_delete(ui_dialog_t *dlg);

/* ========================================================================
 * 内容构建
 * ======================================================================== */

/**
 * @brief 获取弹窗内容容器
 *
 * 调用方在此容器内创建自己的控件 (标签/按钮/进度条等)。
 * 返回的对象在弹窗销毁时一并销毁。
 */
lv_obj_t *ui_dialog_get_content(ui_dialog_t *dlg);

/**
 * @brief 设置面板标题 (可重复调用修改文本)
 */
void ui_dialog_set_title(ui_dialog_t *dlg, const char *title);

/**
 * @brief 设置面板尺寸
 * @param w 宽度 (px), @param h 高度 (px)
 */
void ui_dialog_set_size(ui_dialog_t *dlg, lv_coord_t w, lv_coord_t h);

/* ========================================================================
 * 动画参数
 * ======================================================================== */

/**
 * @brief 设置弹出/关闭动画时长
 * @param dlg          弹窗句柄
 * @param show_duration 弹出动画时长 (ms), 默认 250
 * @param close_duration 关闭动画时长 (ms), 默认 220
 */
void ui_dialog_set_duration(ui_dialog_t *dlg, uint32_t show_duration,
                            uint32_t close_duration);

/**
 * @brief 是否允许点击遮罩关闭 (默认 false)
 *
 * 开启后点击遮罩会执行 ui_dialog_close(dlg, true) (关闭后自动销毁)。
 */
void ui_dialog_set_mask_click_close(ui_dialog_t *dlg, bool enable);

/* ========================================================================
 * 显示 / 关闭
 * ======================================================================== */

/**
 * @brief 弹出并播放入场时间轴动画
 */
void ui_dialog_show(ui_dialog_t *dlg);

/**
 * @brief 关闭弹窗 (反向回放关闭时间轴动画)
 *
 * @param delete_after true 时, 关闭动画播放完毕后自动销毁对话框对象
 *
 * @note 关闭完成后触发 ui_dialog_set_close_cb() 注册的回调;
 *       若在回调中自行调用 ui_dialog_delete(), 请勿再传 delete_after=true。
 */
void ui_dialog_close(ui_dialog_t *dlg, bool delete_after);

/**
 * @brief 注册关闭完成回调
 */
void ui_dialog_set_close_cb(ui_dialog_t *dlg, ui_dialog_close_cb_t cb,
                            void *user_data);

/**
 * @brief 查询弹窗是否处于打开状态 (动画进行中也算打开)
 */
bool ui_dialog_is_open(ui_dialog_t *dlg);

#ifdef __cplusplus
}
#endif

#endif /* UI_DIALOG_H_INCLUDED */
