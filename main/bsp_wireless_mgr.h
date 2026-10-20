/**
 * @file bsp_wireless_mgr.h
 * @brief 无线子系统管理器 - ESP32-P4 + ESP32 从机 (ESP-Hosted SDIO)
 *
 * 背景:
 *   ESP32-P4 本身没有 WiFi/BT 硬件, 通过 ESP-Hosted 组件经 SDIO 与
 *   ESP32 从机通信。SDIO 传输建立 (~1.2s 复位 + 握手) 和两个协议栈
 *   (Bluedroid / WiFi) 的初始化都会消耗大量内存 (Bluedroid 约占
 *   90-130KB 内部 RAM) 和时间, 因此不能再像旧版那样开机全量初始化。
 *
 * 职责:
 *   1. 从机 SDIO 链路生命周期: 首次需要时异步拉起 (复位 GPIO -> esp_hosted_init
 *      -> connect_to_slave), 建立后保持, 供 BT/WiFi 共用。
 *   2. BT / WiFi 协议栈按需异步启停: 所有重活都在独立 worker 线程执行,
 *      UI 线程只投递请求, 立即返回, 不冻结 LVGL。
 *   3. 结果事件通知 UI (启动成功/失败/停止)。
 *   4. 内存回收接口: 注册到 bsp_memory 回收钩子, 内部 RAM 紧张时自动
 *      停掉"已开启但未连接"的协议栈, 释放其占用的内部 RAM。
 *
 * 策略:
 *   - SDIO 链路一旦建立就保持 (空闲时开销极小), 不做链路级拆卸
 *     (ESP-Hosted 传输层无安全的 deinit 路径)。
 *   - BT 与 WiFi 的 SDIO 带宽仲裁沿用 bsp_bt.c 已有机制:
 *     A2DP 连接时 bsp_wifi_set_enabled(false) 停 WiFi, 断开后恢复。
 *   - 回收只停"未连接"的协议栈, 绝不断开正在使用的连接。
 *
 * 使用方法:
 *   app_main 中调用一次 bsp_wireless_mgr_init();
 *   UI (蓝牙/WiFi 页开关) 调用 bsp_wireless_bt_start()/wifi_start() 等接口;
 *   需要同步等待就绪的动作 (扫描/配对) 先调 *_ensure_started()。
 */

#ifndef BSP_WIRELESS_MGR_H
#define BSP_WIRELESS_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 管理器事件 (回调运行在 worker 线程上下文, UI 需自行转到 LVGL 线程) */
typedef enum {
    WLESS_EVT_LINK_UP = 0,        /* SDIO 从机链路已建立 */
    WLESS_EVT_LINK_FAIL,          /* SDIO 从机链路建立失败 (err 为原因) */
    WLESS_EVT_BT_STARTED,         /* 蓝牙协议栈已启动 */
    WLESS_EVT_BT_START_FAIL,      /* 蓝牙协议栈启动失败 */
    WLESS_EVT_BT_STOPPED,         /* 蓝牙协议栈已停止, 内存已回收 */
    WLESS_EVT_WIFI_STARTED,       /* WiFi 已启动 */
    WLESS_EVT_WIFI_START_FAIL,    /* WiFi 启动失败 (含未编译支持) */
    WLESS_EVT_WIFI_STOPPED,       /* WiFi 已停止 */
} wless_event_t;

typedef void (*wless_event_cb_t)(wless_event_t evt, esp_err_t err);

/**
 * @brief 初始化管理器
 *
 * 创建 worker 线程与请求队列, 注册内存回收钩子。
 * 本身不做任何重活, 开机调用不增加启动时间。
 */
esp_err_t bsp_wireless_mgr_init(void);

/** @brief 注册事件回调 (NULL 取消) */
void bsp_wireless_set_event_cb(wless_event_cb_t cb);

/* ---------------- 蓝牙 ---------------- */

/**
 * @brief 异步启动蓝牙 (链路 -> Bluedroid -> A2DP source)
 *
 * 立即返回。完成 (或失败) 通过 WLESS_EVT_BT_STARTED / BT_START_FAIL 通知。
 * 重复调用安全 (已启动时无操作)。
 *
 * @return ESP_OK 请求已受理; ESP_ERR_NO_MEM 队列满; ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t bsp_wireless_bt_start(void);

/**
 * @brief 异步停止蓝牙并回收内存
 *
 * worker 会先断开 A2DP (等待最多 3s), 再完整反初始化 Bluedroid 与
 * HCI 桥接, 释放 PCM ring (256KB PSRAM) 与协议栈内部 RAM。
 */
esp_err_t bsp_wireless_bt_stop(void);

/**
 * @brief 同步确保蓝牙已就绪 (供扫描/配对等动作前调用)
 *
 * 已就绪立即返回; 未就绪则投递启动请求并等待完成。
 * 会阻塞调用线程 (典型 2-3s, 上限 timeout_ms), 只应在用户明确
 * 触发的动作中使用, 不要在 LVGL 渲染路径调用。
 *
 * @param timeout_ms 最长等待毫秒数 (建议 >= 12000)
 * @return ESP_OK 就绪; ESP_ERR_TIMEOUT 超时; 其他为启动失败原因
 */
esp_err_t bsp_wireless_bt_ensure_started(uint32_t timeout_ms);

/** @brief 蓝牙协议栈是否处于已启动状态 */
bool bsp_wireless_bt_active(void);

/* ---------------- WiFi ---------------- */

/** @brief 异步启动 WiFi (链路 -> esp_wifi_remote STA), 事件通知结果 */
esp_err_t bsp_wireless_wifi_start(void);

/** @brief 异步停止 WiFi 并回收内存 */
esp_err_t bsp_wireless_wifi_stop(void);

/** @brief 同步确保 WiFi 已就绪 (语义同 bt_ensure_started) */
esp_err_t bsp_wireless_wifi_ensure_started(uint32_t timeout_ms);

/** @brief WiFi 协议栈是否处于已启动状态 */
bool bsp_wireless_wifi_active(void);

/* ---------------- 链路与内存 ---------------- */

/** @brief SDIO 从机链路是否已建立 */
bool bsp_wireless_link_is_up(void);

/**
 * @brief 内存回收入口 (bsp_memory 回收钩子调用)
 *
 * 将"已开启但未连接"的 BT/WiFi 协议栈停止请求投递给 worker,
 * 立即返回估算可回收的内部 RAM 字节数 (回收实际异步完成)。
 *
 * @param urgency 紧急程度: 0 = 常规, 1 = 紧急 (当前策略相同, 预留)
 * @return 估算可回收的内部 RAM 字节数 (0 表示无需回收)
 */
uint32_t bsp_wireless_reclaim(uint32_t urgency);

#ifdef __cplusplus
}
#endif

#endif /* BSP_WIRELESS_MGR_H */
