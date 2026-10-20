#ifndef UI_GAMES_H
#define UI_GAMES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

void ui_games_init(void);
/* 兼容旧接口，实际实现位于独立游戏模块。 */
void ui_snake_show(void);
void ui_2048_show(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_GAMES_H */
