#include "ui_games.h"
#include "game_snake.h"
#include "game_2048.h"

void ui_snake_show(void)
{
    game_snake_show();
}

void ui_2048_show(void)
{
    game_2048_show();
}

void ui_games_init(void)
{
    /* 游戏模块采用按需初始化，具体页面由各自模块负责。 */
}
