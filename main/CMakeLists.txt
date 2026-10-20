idf_component_register(
    
    SRCS
        "main.c"
        "bsp_sd_card.c"
        "bsp_brightness.c"
        "bsp_audio.c"
        "audio_player.c"
        "audio_decoder.c"
        "id3_tags.c"
        "lrc_parser.c"
        "fft_spectrum.c"
        "bsp_wifi.c"
        "bsp_bt.c"
        "bsp_wireless_mgr.c"
        "ui_wireless_bridge.c"
        "ui_lvgl_async.c"
        "bsp_hw_test.c"
        "bsp_theme.c"
        "ui_gesture.c"
        "ui_desktop.c"
        "ui_music_player.c"
        "ui_settings.c"
        "ui_wifi.c"
        "ui_bluetooth.c"
        "ui_file_manager.c"
        "ui_navbar.c"
        "ui_anim.c"
        "ui_dialog.c"
        "ui_page.c"
        "ui_apps.c"
        "ui_calculator.c"
        "ui_scientific_calculator.c"
        "ui_function_plotter.c"
        "ui_stopwatch.c"
        "ui_games.c"
        "game_snake.c"
        "paoku_game.c"
        "game_2048.cpp"
        "gomoku_game.cpp"
        "go_game.cpp"
        "bsp_memory.c"
        "lv_mem_psram.c"
        "esp_2.41ttf.c"
        "gbk_to_unicode.c"

    INCLUDE_DIRS
        "."
        "lvgl-master/src/libs/lodepng"

    REQUIRES
        esp_driver_sdmmc
        sdmmc
        esp_lcd_co6300
        esp_lcd_touch_cst3530
        esp_lvgl_port
        esp_lvgl_adapter
        lvgl
        esp_wifi
        bt
        nvs_flash
        fatfs
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_hosted
        esp_hosted_hci_bluedroid
        micro_mp3
)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# lv_mem_psram.c 提供 LVGL 自定义分配器 (CONFIG_LV_USE_CUSTOM_MALLOC=y)。
# libmain.a 在链接行先于 lvgl 成员被扫描, 用 -u 预置未定义符号,
# 确保 lv_mem_psram.o 被拉入最终链接 (同 esp_hosted 的 --undefined 用法)。
target_link_libraries(${COMPONENT_LIB} PRIVATE
    "-u lv_mem_init"
    "-u lv_mem_deinit"
    "-u lv_malloc_core"
    "-u lv_realloc_core"
    "-u lv_free_core"
    "-u lv_mem_monitor_core"
    "-u lv_mem_test_core"
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-format-truncation
)
if(CONFIG_BT_ENABLED AND CONFIG_BT_CONTROLLER_DISABLED)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "$ENV{IDF_PATH}/components/bt/include/esp32/include"
    )
endif()
