/*******************************************************************************
 * 通用弹窗组件实现 - LVGL 9.x (ESP-IDF C)
 *
 * 弹出/关闭使用同一套 lv_anim_timeline 时间轴:
 *   - 弹出: 正向播放 (mask 淡入 + panel 缩放/淡入/上移);
 *   - 关闭: 重建时间轴 (时长可独立配置) -> set_progress(MAX) 复位到终点 ->
 *            set_reverse(true) + start 反向回放。
 *
 * 生命周期安全说明:
 *   - 不要在动画 completed_cb 中直接销毁时间轴 (lv_anim_timeline_delete 会
 *     free anim_dsc, 而 anim_timeline_set_act_time() 的循环仍在遍历它 ->
 *     use-after-free);
 *   - 因此关闭收尾只做标志/回调, 真正的销毁通过一次性 lv_timer 延迟 1ms 执行。
 ******************************************************************************/

#include "ui_dialog.h"
#include "ui_anim.h"   /* 复用样式属性 exec 包装 */
#include <stdlib.h>

/* ========================================================================
 * 弹窗句柄结构
 * ======================================================================== */
struct ui_dialog_t {
    lv_obj_t *mask;            /* 全屏遮罩 */
    lv_obj_t *panel;           /* 内容面板 (mask 子对象) */
    lv_obj_t *title;           /* 标题标签 */
    lv_obj_t *content;         /* 内容容器 (调用方控件挂这里) */

    lv_anim_timeline_t *tl;    /* 当前播放的时间轴 */

    uint32_t show_duration;    /* 弹出动画时长 */
    uint32_t close_duration;   /* 关闭动画时长 */

    bool open;                 /* 是否处于打开/播放状态 */
    bool delete_after;         /* 关闭结束后是否自动销毁 */
    bool mask_click_close;     /* 点击遮罩关闭 */

    ui_dialog_close_cb_t close_cb;
    void *close_user_data;
};

/* ========================================================================
 * 内部辅助
 * ======================================================================== */

static void dialog_mask_event_cb(lv_event_t *e);
static void dialog_tl_mask_done(lv_anim_t *a);
static void dialog_delete_timer_cb(lv_timer_t *t);

static bool dialog_build_timeline(ui_dialog_t *dlg, uint32_t duration)
{
    if (dlg->tl) {
        lv_anim_timeline_delete(dlg->tl);
        dlg->tl = NULL;
    }

    lv_anim_timeline_t *tl = lv_anim_timeline_create();
    if (tl == NULL) return false;
    dlg->tl = tl;

    lv_anim_t a;

    /* 1) 遮罩淡入: 0 -> 200
     *    完成回调挂在第一个动画上: 正向播放完成时不处理,
     *    反向 (关闭) 播放到底 (act_time==0) 时执行收尾。 */
    lv_anim_init(&a);
    lv_anim_set_var(&a, dlg->mask);
    lv_anim_set_exec_cb(&a, ui_anim_set_style_opa);
    lv_anim_set_values(&a, 0, 200);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_user_data(&a, dlg);
    lv_anim_set_completed_cb(&a, dialog_tl_mask_done);
    lv_anim_timeline_add(tl, 0, &a);

    /* 2) 面板淡入: 0 -> 255 */
    lv_anim_init(&a);
    lv_anim_set_var(&a, dlg->panel);
    lv_anim_set_exec_cb(&a, ui_anim_set_style_opa);
    lv_anim_set_values(&a, 0, LV_OPA_COVER);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_timeline_add(tl, 0, &a);

    /* 3) 面板上移: 24 -> 0 (轻微"升起"感) */
    lv_anim_init(&a);
    lv_anim_set_var(&a, dlg->panel);
    lv_anim_set_exec_cb(&a, ui_anim_set_style_translate_y);
    lv_anim_set_values(&a, 24, 0);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_timeline_add(tl, 0, &a);

    /* 4) 面板缩放弹出: 0.8x -> 1.0x (overshoot 回弹), 延迟 30ms 增强层次 */
    lv_anim_init(&a);
    lv_anim_set_var(&a, dlg->panel);
    lv_anim_set_exec_cb(&a, ui_anim_set_style_scale_xy);
    lv_anim_set_values(&a, (int32_t)(8 * LV_SCALE_NONE / 10), LV_SCALE_NONE);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_timeline_add(tl, 30, &a);

    return true;
}

/* 时间轴第一个动画 (遮罩淡入) 的完成回调 */
static void dialog_tl_mask_done(lv_anim_t *a)
{
    ui_dialog_t *dlg = (ui_dialog_t *)lv_anim_get_user_data(a);
    if (dlg == NULL) return;

    /* 只在反向播放到底时收尾 */
    if (lv_anim_timeline_get_reverse(dlg->tl)) {
        dlg->open = false;
        lv_obj_add_flag(dlg->mask, LV_OBJ_FLAG_HIDDEN);

        if (dlg->close_cb) {
            dlg->close_cb(dlg, dlg->close_user_data);
        }
        if (dlg->delete_after) {
            /* 延迟销毁: 不能在时间轴回调内释放时间轴本身 */
            lv_timer_t *t = lv_timer_create(dialog_delete_timer_cb, 1, dlg);
            lv_timer_set_repeat_count(t, 1); /* 触发一次后自动删除自身 */
            (void)t;
        }
    }
}

static void dialog_delete_timer_cb(lv_timer_t *t)
{
    ui_dialog_t *dlg = (ui_dialog_t *)lv_timer_get_user_data(t);
    ui_dialog_delete(dlg);
}

/* 点击遮罩 -> 关闭并自动销毁 */
static void dialog_mask_event_cb(lv_event_t *e)
{
    ui_dialog_t *dlg = (ui_dialog_t *)lv_event_get_user_data(e);
    if (dlg == NULL) return;
    if (dlg->mask_click_close) {
        ui_dialog_close(dlg, true);
    }
}

/* ========================================================================
 * 创建 / 销毁
 * ======================================================================== */

ui_dialog_t *ui_dialog_create(lv_obj_t *parent)
{
    ui_dialog_t *dlg = (ui_dialog_t *)lv_malloc_zeroed(sizeof(ui_dialog_t));
    if (dlg == NULL) return NULL;

    if (parent == NULL) parent = lv_layer_top();
    lv_display_t *disp = lv_display_get_default();
    lv_coord_t w = lv_display_get_horizontal_resolution(disp);
    lv_coord_t h = lv_display_get_vertical_resolution(disp);

    /* ---- 全屏遮罩 ---- */
    dlg->mask = lv_obj_create(parent);
    lv_obj_set_size(dlg->mask, w, h);
    lv_obj_set_pos(dlg->mask, 0, 0);
    lv_obj_set_style_bg_color(dlg->mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dlg->mask, LV_OPA_COVER, 0);
    /* 遮罩整体透明度 (LV_STYLE_OPA) 由动画 0->200 控制, 不能改 bg_opa,
     * 否则动画会一直透明看不到背景 */
    lv_obj_set_style_radius(dlg->mask, 0, 0);
    lv_obj_set_style_border_width(dlg->mask, 0, 0);
    lv_obj_set_style_pad_all(dlg->mask, 0, 0);
    lv_obj_clear_flag(dlg->mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dlg->mask, LV_OBJ_FLAG_CLICKABLE); /* 拦截下层触摸 */

    /* ---- 面板 ---- */
    dlg->panel = lv_obj_create(dlg->mask);
    lv_obj_set_size(dlg->panel, 240, 200);
    lv_obj_set_style_bg_color(dlg->panel, lv_color_hex(0x2a2a3a), 0);
    lv_obj_set_style_bg_opa(dlg->panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dlg->panel, 16, 0);
    lv_obj_set_style_border_width(dlg->panel, 0, 0);
    lv_obj_set_style_shadow_width(dlg->panel, 0, 0);   /* 阴影费 CPU, 关闭 */
    lv_obj_set_style_pad_all(dlg->panel, 12, 0);
    lv_obj_set_flex_flow(dlg->panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg->panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(dlg->panel, 8, 0);
    lv_obj_clear_flag(dlg->panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dlg->panel, LV_OBJ_FLAG_CLICKABLE); /* 吸收面板区点击 */

    /* ---- 标题 ---- */
    dlg->title = lv_label_create(dlg->panel);
    lv_label_set_text(dlg->title, "");
    lv_obj_set_style_text_font(dlg->title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(dlg->title, lv_color_hex(0xffffff), 0);

    /* ---- 内容容器 (调用方控件挂这里) ---- */
    dlg->content = lv_obj_create(dlg->panel);
    lv_obj_set_width(dlg->content, lv_pct(100));
    lv_obj_set_flex_grow(dlg->content, 1);
    lv_obj_set_style_bg_opa(dlg->content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dlg->content, 0, 0);
    lv_obj_set_style_radius(dlg->content, 0, 0);
    lv_obj_set_style_pad_all(dlg->content, 0, 0);
    lv_obj_set_style_pad_row(dlg->content, 8, 0);
    lv_obj_set_flex_flow(dlg->content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg->content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(dlg->content, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 默认参数 ---- */
    dlg->show_duration  = 250;
    dlg->close_duration = 220;

    lv_obj_add_event_cb(dlg->mask, dialog_mask_event_cb, LV_EVENT_CLICKED, dlg);

    /* 创建后隐藏, 由 ui_dialog_show() 显示 */
    lv_obj_add_flag(dlg->mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dlg->panel, LV_OBJ_FLAG_HIDDEN);

    return dlg;
}

void ui_dialog_delete(ui_dialog_t *dlg)
{
    if (dlg == NULL) return;

    if (dlg->tl) {
        lv_anim_timeline_delete(dlg->tl); /* 暂停并释放 */
        dlg->tl = NULL;
    }
    if (dlg->mask) {
        lv_obj_delete(dlg->mask);         /* 级联删除 panel/title/content */
        dlg->mask = NULL;
    }
    lv_free(dlg);
}

/* ========================================================================
 * 内容构建
 * ======================================================================== */

lv_obj_t *ui_dialog_get_content(ui_dialog_t *dlg)
{
    if (dlg == NULL) return NULL;
    return dlg->content;
}

void ui_dialog_set_title(ui_dialog_t *dlg, const char *title)
{
    if (dlg == NULL) return;
    lv_label_set_text(dlg->title, title ? title : "");
}

void ui_dialog_set_size(ui_dialog_t *dlg, lv_coord_t w, lv_coord_t h)
{
    if (dlg == NULL) return;
    lv_obj_set_size(dlg->panel, w, h);
    lv_obj_center(dlg->panel);
}

/* ========================================================================
 * 动画参数
 * ======================================================================== */

void ui_dialog_set_duration(ui_dialog_t *dlg, uint32_t show_duration,
                            uint32_t close_duration)
{
    if (dlg == NULL) return;
    if (show_duration)  dlg->show_duration  = show_duration;
    if (close_duration) dlg->close_duration = close_duration;
}

void ui_dialog_set_mask_click_close(ui_dialog_t *dlg, bool enable)
{
    if (dlg == NULL) return;
    dlg->mask_click_close = enable;
}

/* ========================================================================
 * 显示 / 关闭
 * ======================================================================== */

void ui_dialog_show(ui_dialog_t *dlg)
{
    if (dlg == NULL) return;

    /* 重置样式到动画起点, 避免上次残留值 */
    lv_obj_set_style_transform_scale_x(dlg->panel, (int32_t)(8 * LV_SCALE_NONE / 10), 0);
    lv_obj_set_style_transform_scale_y(dlg->panel, (int32_t)(8 * LV_SCALE_NONE / 10), 0);
    lv_obj_set_style_transform_pivot_x(dlg->panel, lv_obj_get_width(dlg->panel) / 2, 0);
    lv_obj_set_style_transform_pivot_y(dlg->panel, lv_obj_get_height(dlg->panel) / 2, 0);
    lv_obj_set_style_translate_y(dlg->panel, 24, 0);
    lv_obj_set_style_opa(dlg->panel, 0, 0);
    lv_obj_set_style_opa(dlg->mask, 0, 0);

    lv_obj_center(dlg->panel);
    lv_obj_clear_flag(dlg->mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(dlg->panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(dlg->mask);   /* 盖在导航栏之上, 模态拦截 */

    if (!dialog_build_timeline(dlg, dlg->show_duration)) return;

    dlg->open = true;
    dlg->delete_after = false;
    lv_anim_timeline_set_reverse(dlg->tl, false);
    lv_anim_timeline_start(dlg->tl);
}

void ui_dialog_close(ui_dialog_t *dlg, bool delete_after)
{
    if (dlg == NULL) return;

    dlg->delete_after = delete_after;

    if (!dlg->open) {
        /* 未打开或已关闭: 直接销毁 (或什么都不做) */
        if (delete_after) ui_dialog_delete(dlg);
        return;
    }

    /* 用关闭时长重建时间轴, 再从终点反向播放 */
    if (!dialog_build_timeline(dlg, dlg->close_duration)) {
        dlg->open = false;
        lv_obj_add_flag(dlg->mask, LV_OBJ_FLAG_HIDDEN);
        if (dlg->close_cb) dlg->close_cb(dlg, dlg->close_user_data);
        if (delete_after) ui_dialog_delete(dlg);
        return;
    }

    lv_anim_timeline_set_progress(dlg->tl, LV_ANIM_TIMELINE_PROGRESS_MAX);
    lv_anim_timeline_set_reverse(dlg->tl, true);
    lv_anim_timeline_start(dlg->tl);
    /* open 保持 true, 反向播放到底后由 dialog_tl_mask_done 置 false */
}

void ui_dialog_set_close_cb(ui_dialog_t *dlg, ui_dialog_close_cb_t cb,
                            void *user_data)
{
    if (dlg == NULL) return;
    dlg->close_cb = cb;
    dlg->close_user_data = user_data;
}

bool ui_dialog_is_open(ui_dialog_t *dlg)
{
    if (dlg == NULL) return false;
    return dlg->open;
}
