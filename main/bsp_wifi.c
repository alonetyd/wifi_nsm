/**
 * @file bsp_wifi.c
 * @brief WiFi模块 - 通过ESP-Hosted SDIO连接WiFi
 *
 * ESP32-P4通过ESP-Hosted组件经SDIO与协处理器通信。
 * 需要在menuconfig中启用: CONFIG_ESP_HOST_WIFI_ENABLED=y
 * 未启用时所有API返回ESP_ERR_NOT_SUPPORTED。
 */

#include "bsp_wifi.h"
#include "bsp_board.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"

#if CONFIG_ESP_HOST_WIFI_ENABLED

/* 注: 必须先包含 injected/esp_wifi.h 才能让 WIFI_REMOTE_EVENT 等宏正确生效,
 * 这里通过 esp_wifi_remote.h 间接包含即可。
 */
#include "esp_wifi_remote.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"

static const char *TAG = "bsp_wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      10
#define WIFI_IP_STR_MAX_LEN 16

static bool s_initialized = false;
static bool s_connected = false;
static bool s_wifi_enabled = true;   /* Wi-Fi 总开关（BT 共存时关闭） */
static bool s_scanning = false;      /* 正在扫描 (内存回收时保护) */
static char s_ip_str[WIFI_IP_STR_MAX_LEN] = {0};
static int s_retry_count = 0;
static EventGroupHandle_t s_event_group = NULL;
static wifi_event_cb_t s_event_cb = NULL;
static esp_netif_t *s_sta_netif = NULL;

/* 扫描结果存储 */
static wifi_ap_info_t s_scan_results[WIFI_SCAN_MAX];
static int s_scan_count = 0;

/* 扫描完成回调 - 调度到 LVGL 线程 */
typedef void (*scan_done_cb_t)(void);
static scan_done_cb_t s_scan_done_cb = NULL;

/* WiFi配置保存路径 */
#define WIFI_CONFIG_FILE "/sdcard/wifi.cfg"

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    /* WIFI_REMOTE_EVENT 在未启用本地 WiFi 时与 WIFI_EVENT 等价 */
    if (event_base == WIFI_REMOTE_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA已启动 (远端)");
            /* 不在此自动连接, 等待用户在UI中选网 */
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "WiFi已关联到AP");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            s_ip_str[0] = '\0';
            /* s_wifi_enabled=false 说明 WiFi 正在被主动停用 (deinit 或
             * BT 共存), 此时不要重连, 让 stop/deinit 流程干净落地 */
            if (s_wifi_enabled && s_retry_count < WIFI_MAX_RETRY) {
                ESP_LOGW(TAG, "WiFi断开，重连(%d/%d)", s_retry_count + 1, WIFI_MAX_RETRY);
                esp_wifi_connect();
                s_retry_count++;
            } else {
                ESP_LOGE(TAG, "WiFi连接失败，已达最大重连次数");
                xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
                if (s_event_cb) s_event_cb(false, NULL);
            }
            break;
        case WIFI_EVENT_SCAN_DONE:
            s_scanning = false;
            ESP_LOGI(TAG, "WiFi扫描完成");
            {
                uint16_t num = 0;
                wifi_ap_record_t *ap_info = NULL;
                esp_wifi_scan_get_ap_num(&num);
                ESP_LOGI(TAG, "扫描发现 %u 个AP", num);
                if (num > WIFI_SCAN_MAX) num = WIFI_SCAN_MAX;
                if (num > 0) {
                    ap_info = heap_caps_malloc(num * sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
                    if (ap_info) {
                        memset(ap_info, 0, num * sizeof(wifi_ap_record_t));
                        uint16_t ap_count = num;
                        if (esp_wifi_scan_get_ap_records(&ap_count, ap_info) == ESP_OK) {
                            s_scan_count = ap_count;
                            for (int i = 0; i < ap_count && i < WIFI_SCAN_MAX; i++) {
                                strncpy(s_scan_results[i].ssid, (char *)ap_info[i].ssid, WIFI_SSID_MAX);
                                s_scan_results[i].ssid[WIFI_SSID_MAX] = '\0';
                                s_scan_results[i].rssi = ap_info[i].rssi;
                                s_scan_results[i].authmode = ap_info[i].authmode;
                                s_scan_results[i].saved = false;
                            }
                            ESP_LOGI(TAG, "获取到 %d 个WiFi网络", s_scan_count);
                        } else {
                            ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records 失败");
                            s_scan_count = 0;
                        }
                        free(ap_info);
                    } else {
                        ESP_LOGE(TAG, "PSRAM分配扫描结果失败");
                    }
                } else {
                    s_scan_count = 0;
                }

                /* 通知UI (LVGL异步回调, 安全) */
                if (s_scan_done_cb) {
                    s_scan_done_cb();
                }
            }
            break;
        default:
            ESP_LOGD(TAG, "未处理的WiFi事件: %ld", event_id);
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "获取IP: %s", s_ip_str);
            s_connected = true;
            s_retry_count = 0;
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
            if (s_event_cb) s_event_cb(true, s_ip_str);
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            s_connected = false;
            s_ip_str[0] = '\0';
            ESP_LOGW(TAG, "IP地址丢失");
        }
    }
}

#endif /* CONFIG_ESP_HOST_WIFI_ENABLED */

esp_err_t bsp_wifi_init(void)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    if (s_initialized) {
        ESP_LOGW(TAG, "WiFi已初始化");
        return ESP_OK;
    }

    s_event_group = xEventGroupCreate();
    if (!s_event_group) return ESP_ERR_NO_MEM;

    /* 创建默认事件循环(如果尚未创建) */
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    /* ESP-Hosted: 必须使用 esp_wifi_remote_create_default_sta() 创建网络接口,
     * 传统的 esp_netif_create_default_wifi_sta() 不会绑定到远程 WiFi 控制器。 */
    s_sta_netif = esp_wifi_remote_create_default_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "esp_wifi_remote_create_default_sta 失败");
        return ESP_FAIL;
    }

    /* ESP-Hosted: 使用 ESP_HOSTED 默认配置初始化 WiFi 远端 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 注册事件: 使用 WIFI_REMOTE_EVENT (CONFIG_ESP_WIFI_ENABLED 未定义时等于 WIFI_EVENT) */
    esp_event_handler_instance_register(WIFI_REMOTE_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler, NULL, NULL);

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;

    s_wifi_enabled = true;   /* deinit 时被清掉, 重新初始化要恢复 */
    s_initialized = true;
    ESP_LOGI(TAG, "WiFi初始化完成 (ESP-Hosted SDIO, STA netif=%p)", s_sta_netif);
    return ESP_OK;
#else
    ESP_LOGE("bsp_wifi", "WiFi未启用，请在menuconfig中启用 ESP-Hosted WiFi");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bsp_wifi_deinit(void)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    if (!s_initialized) return ESP_OK;

    /* 1. 断开连接 (事件回调会因 retry 逻辑尝试重连, 先加锁标志) */
    s_wifi_enabled = false;          /* 阻止 disconnect 重连循环 */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));  /* 让断开事件落地 */

    /* 2. 停止并反初始化 WiFi (释放 esp_wifi_remote 占用的内部 RAM) */
    esp_wifi_stop();
    esp_wifi_deinit();

    /* 3. 注销事件处理 */
    esp_event_handler_instance_unregister(WIFI_REMOTE_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler);

    /* 清理状态 */
    if (s_event_group) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }
    s_initialized = false;
    s_connected = false;
    s_scanning = false;
    s_retry_count = 0;
    s_scan_count = 0;
    s_ip_str[0] = '\0';

    /* STA netif 保留: esp_wifi_remote 无对称销毁接口, 且占用很小,
     * 下次 bsp_wifi_init 时复用. */
    ESP_LOGI(TAG, "WiFi 已反初始化, 协议栈内存已回收");
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bsp_wifi_connect(const char *ssid, const char *password)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!ssid) return ESP_ERR_INVALID_ARG;

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) return ret;

    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;

    ret = esp_wifi_connect();
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "正在连接WiFi: %s", ssid);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bsp_wifi_disconnect(void)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = esp_wifi_disconnect();
    if (ret == ESP_OK) {
        s_connected = false;
        s_ip_str[0] = '\0';
        s_retry_count = 0;
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
    }
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool bsp_wifi_is_connected(void)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    return s_connected;
#else
    return false;
#endif
}

bool bsp_wifi_is_scanning(void)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    return s_scanning;
#else
    return false;
#endif
}

const char *bsp_wifi_get_ip(void)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    return (s_connected && s_ip_str[0]) ? s_ip_str : NULL;
#else
    return NULL;
#endif
}

void bsp_wifi_set_event_cb(wifi_event_cb_t cb)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    s_event_cb = cb;
#else
    (void)cb;
#endif
}

void bsp_wifi_set_ps_for_bt(bool bt_active)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    if (!s_initialized) return;
    /* BT 播放时把 WiFi 拉入最省电模式, 让 SDIO 流量集中在 BT HCI.
     * 注意: 这是 ESP-Hosted 远程 WiFi, 实际指令会通过 SDIO 发到协处理器.
     * 但因为我们的目的就是减少 SDIO 流量, 所以这是"双刃剑" - 恢复
     * 时需要小心. 我们只在 BT 已连接 (A2DP 推送 PCM 时) 进入 PS,
     * BT 断开时立刻退出. */
    wifi_ps_type_t ps = bt_active ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE;
    esp_err_t ret = esp_wifi_set_ps(ps);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "设置 WiFi PS=%d 失败: %s", (int)ps, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "WiFi PS=%d (bt_active=%d)", (int)ps, (int)bt_active);
    }
#else
    (void)bt_active;
#endif
}

void bsp_wifi_set_enabled(bool enabled)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    if (!s_initialized) return;
    if (s_wifi_enabled == enabled) return;  /* 避免重复操作 */
    s_wifi_enabled = enabled;
    if (enabled) {
        /* BT 断开后恢复 Wi-Fi */
        esp_err_t ret = esp_wifi_start();
        ESP_LOGI(TAG, "WiFi 恢复: %s",
                  ret == ESP_OK ? "已启动" : esp_err_to_name(ret));
    } else {
        /* BT 已连接，停止 Wi-Fi 让 SDIO 100% 给 BT 用。
         * stop 失败时降级为 PS_MIN_MODEM。 */
        esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "WiFi 停止失败，降级为 PS_MIN_MODEM: %s",
                      esp_err_to_name(ret));
            s_wifi_enabled = true;
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        } else {
            ESP_LOGI(TAG, "WiFi 已停止 (BT 共存)");
        }
    }
#else
    (void)enabled;
#endif
}

void bsp_wifi_set_scan_done_cb(void (*cb)(void))
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    s_scan_done_cb = cb;
#else
    (void)cb;
#endif
}

esp_err_t bsp_wifi_scan(void)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    s_scan_count = 0;
    /* 扫描配置: 全信道扫描, 显示隐藏 AP, 主动扫描 */
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,           /* 0 = 全信道扫描 */
        .show_hidden = true,    /* 显示隐藏AP */
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t ret = esp_wifi_scan_start(&scan_config, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start 失败: %s", esp_err_to_name(ret));
        s_scanning = false;
    } else {
        s_scanning = true;
        ESP_LOGI(TAG, "WiFi扫描已启动 (主动扫描)");
    }
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

int bsp_wifi_get_scan_results(wifi_ap_info_t *aps, int max_cnt)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    if (!aps || max_cnt <= 0) return 0;
    int cnt = (s_scan_count < max_cnt) ? s_scan_count : max_cnt;
    memcpy(aps, s_scan_results, cnt * sizeof(wifi_ap_info_t));
    return cnt;
#else
    return 0;
#endif
}

esp_err_t bsp_wifi_save_ap(const char *ssid, const char *password)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    if (!ssid) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(WIFI_CONFIG_FILE, "a");
    if (!f) return ESP_FAIL;

    fprintf(f, "%s|%s\n", ssid, password ? password : "");
    fflush(f);
    fclose(f);

    ESP_LOGI(TAG, "WiFi配置已保存: %s", ssid);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

int bsp_wifi_load_saved_aps(wifi_saved_ap_t *aps, int max_cnt)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    if (!aps || max_cnt <= 0) return 0;

    FILE *f = fopen(WIFI_CONFIG_FILE, "r");
    if (!f) return 0;

    int cnt = 0;
    char line[256];
    while (cnt < max_cnt && fgets(line, sizeof(line), f)) {
        char *ssid = strtok(line, "|");
        char *pass = strtok(NULL, "\n");
        if (ssid) {
            strncpy(aps[cnt].ssid, ssid, WIFI_SSID_MAX);
            aps[cnt].ssid[WIFI_SSID_MAX] = '\0';
            if (pass) {
                strncpy(aps[cnt].password, pass, WIFI_PASS_MAX);
                aps[cnt].password[WIFI_PASS_MAX] = '\0';
            } else {
                aps[cnt].password[0] = '\0';
            }
            cnt++;
        }
    }
    fclose(f);
    return cnt;
#else
    return 0;
#endif
}

esp_err_t bsp_wifi_remove_saved_ap(const char *ssid)
{
#if CONFIG_ESP_HOST_WIFI_ENABLED
    if (!ssid) return ESP_ERR_INVALID_ARG;

    /* 读取所有配置 */
    wifi_saved_ap_t aps[20];
    int cnt = bsp_wifi_load_saved_aps(aps, 20);

    /* 重新写入，排除要删除的 */
    FILE *f = fopen(WIFI_CONFIG_FILE, "w");
    if (!f) return ESP_FAIL;

    for (int i = 0; i < cnt; i++) {
        if (strcmp(aps[i].ssid, ssid) != 0) {
            fprintf(f, "%s|%s\n", aps[i].ssid, aps[i].password);
        }
    }
    fflush(f);
    fclose(f);

    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
