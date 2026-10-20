#ifndef UI_APPS_H
#define UI_APPS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化应用列表页面和桌面底部上滑触发区域。 */
void ui_apps_init(void);

/* 显示应用列表页面。 */
void ui_apps_show(void);

/* 在指定桌面上添加底部向上滑动触发区域。 */
void ui_apps_add_swipe_trigger(lv_obj_t *desktop);

#ifdef __cplusplus
}
#endif

#endif /* UI_APPS_H */
