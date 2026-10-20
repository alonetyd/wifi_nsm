# [OPEN] UI startup crash

## Symptom
设备完成 LCD、触摸、SD 卡、音频和音乐扫描后，在 UI 初始化阶段出现：
`Guru Meditation Error: Core 0 panic'ed (Store access fault)`
随后反复重启。

## Hypotheses
1. `ui_desktop_init()` 内部发生非法写入。
2. `ui_apps_init()` 内部发生非法写入。
3. `ui_games_init()` 预创建游戏页面时发生非法写入。
4. `ui_navbar_init()` 创建顶部层或导航按钮时发生非法写入。

## Evidence before instrumentation
已知最后一条稳定日志为：
`Main: 已推送 3 首音乐到播放列表`

## Evidence after startup-stage instrumentation
已确认：
- `UI_INIT_DESKTOP_DONE` 已输出，排除桌面初始化作为当前崩溃点。
- `UI_INIT_APPS_DONE` 已输出，排除应用列表初始化作为当前崩溃点。
- 最后一条日志为 `UI_INIT_GAMES_BEGIN`，崩溃发生在 `ui_games_init()` 内部。

当前仍需通过游戏模块内部日志区分贪吃蛇初始化和 2048 初始化的具体位置。

## Evidence from game-initialization instrumentation
已确认最后日志为：
- `ui_games: SNAKE_SCREEN_DONE`

说明 `lv_obj_create(NULL)` 创建贪吃蛇屏幕后，在继续创建页面控件的阶段发生崩溃。2048 页面尚未开始创建。

## Fix applied
`ui_games_init()` 已改为空初始化，贪吃蛇和 2048 页面改为用户点击后按需创建，避免启动阶段创建游戏页面及大量 LVGL 对象。

## Status
Waiting for user verification after lazy game initialization fix.
