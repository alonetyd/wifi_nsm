#ifndef PAOKU_GAME_H
#define PAOKU_GAME_H

#ifdef __cplusplus
extern "C" {
#endif

/* 启动裸渲染跑酷游戏；调用后接管当前 MIPI 屏幕。 */
void paoku_game_start(void);

/* 请求退出跑酷游戏，释放资源并恢复 LVGL 桌面。 */
void paoku_game_exit(void);

#ifdef __cplusplus
}
#endif

#endif /* PAOKU_GAME_H */
