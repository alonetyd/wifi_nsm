/**
 * @file bsp_wifi.h
 * @brief WiFi模块 - 通过ESP-Hosted SDIO连接WiFi
 *
 * ESP32-P4本身没有WiFi硬件，通过ESP-Hosted组件经SDIO与协处理器
 * (ESP32-C6/S3)通信，使用esp_wifi_remote API实现WiFi功能。
 * SDIO引脚配置通过menuconfig的ESP-Hosted配置项设置。
 */

#ifndef BSP_WIFI_H
#define BSP_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi扫描结果 */
#define WIFI_SCAN_MAX  20
#define WIFI_SSID_MAX  32
#define WIFI_PASS_MAX  64

typedef struct {
    char ssid[WIFI_SSID_MAX + 1];
    int8_t rssi;
    uint8_t authmode;  /* WPA/WPA2/WPA3等 */
    bool saved;         /* 是否已保存密码 */
} wifi_ap_info_t;

/* WiFi已保存网络 */
typedef struct {
    char ssid[WIFI_SSID_MAX + 1];
    char password[WIFI_PASS_MAX + 1];
} wifi_saved_ap_t;

/**
 * @brief WiFi事件回调类型
 * @param connected true表示已连接，false表示已断开
 * @param ip        连接成功时为获取到的IP地址字符串，断开时为NULL
 */
typedef void (*wifi_event_cb_t)(bool connected, const char *ip);

/**
 * @brief 初始化WiFi (通过ESP-Hosted)
 *
 * 创建默认事件循环、STA网络接口，初始化并启动WiFi。
 * SDIO引脚由ESP-Hosted组件通过menuconfig配置管理。
 *
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t bsp_wifi_init(void);

/**
 * @brief 反初始化WiFi并回收内存 (给 bsp_wireless_mgr 按需启停用)
 *
 * 执行: 断开连接 -> esp_wifi_stop -> esp_wifi_deinit, 注销事件处理并
 * 清空扫描缓存。STA netif 保留 (esp_wifi_remote 未提供对称的销毁接口,
 * 且其占用量很小), 复用 bsp_wifi_init 时直接复用。
 *
 * @return ESP_OK 成功, ESP_ERR_NOT_SUPPORTED 表示编译未启用 WiFi
 */
esp_err_t bsp_wifi_deinit(void);

/**
 * @brief 连接WiFi
 *
 * @param ssid     目标SSID
 * @param password WiFi密码，无密码时传NULL
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t bsp_wifi_connect(const char *ssid, const char *password);

/**
 * @brief 断开WiFi
 *
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t bsp_wifi_disconnect(void);

/**
 * @brief 获取连接状态
 * @return true已连接，false未连接
 */
bool bsp_wifi_is_connected(void);

/**
 * @brief 是否正在扫描
 * @return true扫描进行中 (内存回收等场景不应停掉协议栈)
 */
bool bsp_wifi_is_scanning(void);

/**
 * @brief 获取IP地址
 * @return IP地址字符串，未连接时返回NULL
 */
const char *bsp_wifi_get_ip(void);

/**
 * @brief 设置事件回调
 * @param cb 回调函数指针
 */
void bsp_wifi_set_event_cb(wifi_event_cb_t cb);

/**
 * @brief 为 BT 共存而调整 WiFi 电源模式.
 *
 * 背景: P4 + ESP-Hosted (SDIO) 同时跑 WiFi + BT 时, SDIO 链路高负载会
 *       锁死 ("invalid ret or len_from_slave: 0 0"). A2DP 播放时进入
 *       BT_PS_MIN_MODEM, 让 WiFi 不发 beacon, 释放 SDIO 流量给 BT HCI.
 *
 * @param bt_active true: 进入最低 WiFi 流量 (WIFI_PS_MIN_MODEM).
 *                  false: 退出 PS (回到 NONE).
 */
void bsp_wifi_set_ps_for_bt(bool bt_active);

/**
 * @brief 完全开关 Wi-Fi (给 BT 让 SDIO 带宽).
 *
 * @param enabled true: 启动 Wi-Fi; false: 停止 Wi-Fi.
 *
 * BT 播放时关闭 Wi-Fi 可以让 SDIO 链路 100% 给 BT HCI 使用，
 * 消除 `eh_sdio: invalid ret` 和 `btc_media_aa_prep_sbc_2_send underflow`。
 */
void bsp_wifi_set_enabled(bool enabled);

/**
 * @brief 扫描WiFi
 *
 * 启动异步扫描，扫描结果通过WIFI_EVENT_SCAN_DONE事件通知。
 *
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t bsp_wifi_scan(void);

/**
 * @brief 获取扫描结果
 * @param aps 输出数组
 * @param max_cnt 最大数量
 * @return 实际获取到的数量
 */
int bsp_wifi_get_scan_results(wifi_ap_info_t *aps, int max_cnt);

/**
 * @brief 保存WiFi网络到SD卡
 */
esp_err_t bsp_wifi_save_ap(const char *ssid, const char *password);

/**
 * @brief 设置WiFi扫描完成回调
 *
 * 注意: 该回调在WiFi事件任务中触发, 不要在回调中直接调用LVGL API。
 * 应使用 lv_async_call() 调度到LVGL线程, 或仅设置标志位供LVGL timer轮询。
 *
 * @param cb 回调函数指针, NULL表示取消注册
 */
void bsp_wifi_set_scan_done_cb(void (*cb)(void));

/**
 * @brief 加载已保存的WiFi网络
 */
int bsp_wifi_load_saved_aps(wifi_saved_ap_t *aps, int max_cnt);

/**
 * @brief 删除已保存的WiFi网络
 */
esp_err_t bsp_wifi_remove_saved_ap(const char *ssid);

#ifdef __cplusplus
}
#endif

#endif /* BSP_WIFI_H */
