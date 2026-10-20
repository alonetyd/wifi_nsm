/*******************************************************************************
 * 安卓风格三键导航栏 UI 模块 - LVGL 9.x
 *
 * 适用于 ESP32-P4 项目，屏幕分辨率 262×928（竖屏）
 *
 * 提供类似安卓手机的三键导航:
 *   - 返回键 (Back):  返回上一级页面
 *   - 主页键 (Home):  回到桌面
 *   - 后台键 (Recents): 显示最近应用列表
 *
 * 使用全局 layer_top 作为常驻浮层, 不随屏幕切换而消失。
 * 内部维护一个屏幕栈, 用于实现返回与最近应用功能。
 ******************************************************************************/
#ifndef UI_NAVBAR_H_INCLUDED
#define UI_NAVBAR_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdbool.h>

#define UI_NAVBAR_H 52

/**
 * @brief 初始化导航栏
 *
 * 创建三键导航栏浮层并附加到当前显示的 layer_top。
 * 需在持有 LVGL 锁时调用, 通常在 ui_desktop_init() 之后调用。
 */
void ui_navbar_init(void);

/**
 * @brief 显示导航栏 (带动画)
 */
void ui_navbar_show(void);

/**
 * @brief 隐藏导航栏 (带动画, 例如全屏应用时)
 */
void ui_navbar_hide(void);

/**
 * @brief 切换导航栏可见性
 */
void ui_navbar_toggle(void);

/**
 * @brief 查询导航栏是否可见
 */
bool ui_navbar_is_visible(void);

/**
 * @brief 压入新屏幕到导航栈
 *
 * 由各子页面在显示自己之前调用。会保存当前活动屏幕到栈中,
 * 然后加载新屏幕。返回时通过 ui_navbar_back() 弹出。
 *
 * @param new_scr  即将显示的新屏幕对象
 * @param anim     是否使用过渡动画
 */
void ui_navbar_push_screen(lv_obj_t *new_scr, bool anim);

/**
 * @brief 压入新屏幕, 并指定其在"最近应用"列表中显示的名称
 *
 * @param new_scr  即将显示的新屏幕对象
 * @param anim     是否使用过渡动画
 * @param name     应用名称 (显示在最近应用卡片上), 可为 NULL
 */
void ui_navbar_push_screen_named(lv_obj_t *new_scr, bool anim, const char *name);

/**
 * @brief 替换栈顶屏幕 (不增加栈深度)
 *
 * 用于在同级页面间切换 (例如设置中的子页面切换)。
 */
void ui_navbar_replace_top(lv_obj_t *new_scr, bool anim);

/**
 * @brief 返回上一级屏幕 (返回键功能)
 *
 * 弹出栈顶, 加载栈中上一个屏幕。如果栈为空, 则回到桌面。
 */
void ui_navbar_back(void);

/**
 * @brief 回到桌面 (主页键功能)
 *
 * 清空导航栈并加载桌面屏幕。保留栈中所有屏幕作为"最近应用"。
 */
void ui_navbar_home(void);

/**
 * @brief 显示最近应用列表 (后台键功能)
 *
 * 弹出半透明遮罩, 显示当前导航栈中所有屏幕的缩略图卡片,
 * 用户可点击任意卡片切换到对应应用, 或点击空白处关闭。
 */
void ui_navbar_show_recents(void);

/**
 * @brief 隐藏最近应用列表
 */
void ui_navbar_hide_recents(void);

/**
 * @brief 查询最近应用列表是否正在显示
 */
bool ui_navbar_recents_is_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_NAVBAR_H */
