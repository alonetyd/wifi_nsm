idf_component_register(SRCS "esp_lcd_co6300.c" "esp_lcd_co6300_spi.c" "esp_lcd_co6300_mipi.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       REQUIRES "esp_lcd" "driver")
