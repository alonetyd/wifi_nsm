/*
 * ESP32 Coprocessor - BT Controller Only Firmware
 * 
 * This firmware runs on the ESP32 and provides Bluetooth HCI functionality
 * over SDIO to the ESP32-P4 host.
 * 
 * Flash this firmware to your ESP32 first, then use Bluetooth on ESP32-P4.
 */

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "esp32_cp_bt";

void app_main(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "ESP32 Coprocessor - BT Controller Firmware");
    ESP_LOGI(TAG, "===========================================");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    // BT controller features are auto-initialized by ESP-Hosted CP framework
    // No additional code needed - the framework handles all BT setup

    ESP_LOGI(TAG, "BT controller ready, waiting for host...");
    
    // Keep the task alive
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "BT controller still running...");
    }
}
