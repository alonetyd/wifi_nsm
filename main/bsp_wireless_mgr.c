/**
 * @file bsp_wireless_mgr.c
 * @brief 无线子系统管理器实现 - 从机链路 + BT/WiFi 按需异步启停
 *
 * 线程模型:
 *   - UI/其他线程只投递请求 (队列), 立即返回;
 *   - 唯一的 worker 线程序列化执行: 链路拉起 -> 协议栈 init/deinit;
 *   - 同步接口 (*_ensure_started) 通过事件组等待 worker 完成对应请求。
 */

#include "bsp_wireless_mgr.h"
#include "bsp_board.h"
#include "bsp_bt.h"
#include "bsp_wifi.h"
#include "bsp_memory.h"
#include "esp_hosted.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#if (esp_idf_version >= ESP_IDF_VERSION_VAL(5, 2, 0))
#include "freertos/idf_additions.h"
#endif

static const char *TAG = "wless_mgr";

/* ================ 配置 ================ */

#define WLESS_QUEUE_LEN          8
#define WLESS_TASK_STACK_BYTES   (10 * 1024)
#define WLESS_TASK_PRIO          3
#define WLESS_LINK_SETTLE_MS     1200      /* 从机复位后启动等待 */
#define WLESS_LINK_RETRY         2         /* 链路建立尝试次数 */
#define WLESS_BT_DISCONNECT_WAIT_MS 3000   /* 停蓝牙时等待 A2DP 断开 */
#define WLESS_BT_TEARDOWN_GRACE_MS  300    /* 断开后给 audio_player 切走 BT 路径的缓冲 */

/* 协议栈启动前的内存门禁:
 * Bluedroid init 需要大量内部 (DMA 可用) RAM。内部内存不足/碎片化时,
 * 第一条 HCI 命令 (Reset) 的 DMA 分配就会失败 -> 8s HCI 超时挂死。
 * 两级判定:
 *   达标: free>=64KB 且 内部DMA最大块>=32KB -> 直接启动
 *   勉强: free>=48KB 且 内部DMA最大块>=16KB -> 告警后启动
 *     (蓝牙栈已配 PSRAM 优先分配, 只要不是碎片到极小分配都失败就行)
 *   不足: 先触发回收再复查, 仍不足则快速失败 */
#define BT_MIN_FREE_INTERNAL_KB   64   /* 蓝牙: 内部RAM空闲达标值 */
#define BT_MIN_LARGEST_DMA_KB     32   /* 蓝牙: 内部DMA最大块达标值 */
#define BT_SOFT_FREE_INTERNAL_KB  48   /* 蓝牙: 内部RAM空闲下限 */
#define BT_SOFT_LARGEST_DMA_KB    16   /* 蓝牙: 内部DMA最大块下限 */
#define WIFI_MIN_FREE_INTERNAL_KB 32   /* WiFi: 内部RAM空闲下限 */
#define WLESS_RAM_RETRY           2    /* 回收重试次数 */

/* 事件组位 */
#define EV_BT_START_DONE   BIT0   /* BT 启动请求处理完 (成功或失败) */
#define EV_BT_START_OK     BIT1
#define EV_WIFI_START_DONE BIT2
#define EV_WIFI_START_OK   BIT3

/* ================ 状态 ================ */

typedef enum {
    WLESS_REQ_BT_START = 0,
    WLESS_REQ_BT_STOP,
    WLESS_REQ_WIFI_START,
    WLESS_REQ_WIFI_STOP,
} wless_req_type_t;

typedef struct {
    wless_req_type_t type;
} wless_req_msg_t;

static QueueHandle_t      s_req_queue = NULL;
static TaskHandle_t       s_task = NULL;
static EventGroupHandle_t s_events = NULL;
static wless_event_cb_t   s_event_cb = NULL;

/* 以下状态只在 worker 线程写, 其他线程读 (bool/枚举读写原子) */
static volatile bool s_link_up      = false;   /* SDIO 链路已建立 */
static volatile bool s_bt_active    = false;   /* BT 协议栈已启动 */
static volatile bool s_wifi_active  = false;   /* WiFi 协议栈已启动 */

static bool s_mgr_initialized = false;

/* ================ 事件通知 ================ */

static void notify(wless_event_t evt, esp_err_t err)
{
    if (s_event_cb) {
        s_event_cb(evt, err);
    }
}

/* ================ 从机链路拉起 ================ */

/**
 * @brief 启动前内存门禁 (只在 worker 线程调用)
 *
 * 内部 RAM / DMA 最大块不足时先触发一轮回收再复查;
 * 勉强达标则告警后放行, 仍不足返回 false (调用方快速失败)。
 */
static bool ensure_host_ram(bool for_bt)
{
    for (int attempt = 1; attempt <= WLESS_RAM_RETRY; attempt++) {
        uint32_t free_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024;
        /* 只统计内部 RAM 的 DMA 最大块 (PSRAM-DMA 分区不带 INTERNAL 位, 不会混入) */
        uint32_t dma_kb  = heap_caps_get_largest_free_block(
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT) / 1024;
        /* PSRAM-DMA 分区最大块: 诊断 esp-hosted 缓冲是否真的进了 PSRAM */
        uint32_t pdma_kb = heap_caps_get_largest_free_block(
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT) / 1024;

        if (for_bt) {
            if (free_kb >= BT_MIN_FREE_INTERNAL_KB && dma_kb >= BT_MIN_LARGEST_DMA_KB) return true;
            if (free_kb >= BT_SOFT_FREE_INTERNAL_KB && dma_kb >= BT_SOFT_LARGEST_DMA_KB) {
                ESP_LOGW(TAG, "蓝牙: 内部RAM偏紧 (free=%uKB 内部DMA最大块=%uKB PSRAM-DMA最大块=%uKB), 尝试启动",
                         (unsigned)free_kb, (unsigned)dma_kb, (unsigned)pdma_kb);
                return true;
            }
        } else {
            if (free_kb >= WIFI_MIN_FREE_INTERNAL_KB) return true;
        }

        ESP_LOGW(TAG, "%s启动前内部RAM不足: free=%uKB 内部DMA最大块=%uKB PSRAM-DMA最大块=%uKB, 触发回收 (第%d次)",
                 for_bt ? "蓝牙" : "WiFi",
                 (unsigned)free_kb, (unsigned)dma_kb, (unsigned)pdma_kb, attempt);
        bsp_memory_trim_caches(1);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ESP_LOGE(TAG, "内部RAM仍不足, 取消%s启动 (避免协议栈初始化中途卡死)",
             for_bt ? "蓝牙" : "WiFi");
    return false;
}

/**
 * @brief 建立与 ESP32 从机的 SDIO 链路 (只在 worker 线程调用)
 *
 * 流程与旧版 main.c wifi_bt_init_task 一致 (该流程已验证可跑通):
 *   GPIO 复位从机 -> 等 1.2s -> esp_hosted_init -> connect_to_slave
 */
static esp_err_t ensure_link_up(void)
{
    if (s_link_up) return ESP_OK;

    for (int attempt = 1; attempt <= WLESS_LINK_RETRY; attempt++) {
        ESP_LOGI(TAG, "拉起从机链路 (第 %d/%d 次)...", attempt, WLESS_LINK_RETRY);

        /* 1. 硬件复位 ESP32 从机 */
        gpio_config_t rst_conf = {
            .pin_bit_mask = 1ULL << ESP_HOSTED_SDIO_RST,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_conf);
        gpio_set_level(ESP_HOSTED_SDIO_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(ESP_HOSTED_SDIO_RST, 1);

        /* 2. 等待从机启动 */
        vTaskDelay(pdMS_TO_TICKS(WLESS_LINK_SETTLE_MS));

        /* 3. 初始化 ESP-Hosted 传输层 */
        int ret = esp_hosted_init();
        if (ret != 0) {
            ESP_LOGE(TAG, "esp_hosted_init 失败: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* 4. 连接从机 */
        ret = esp_hosted_connect_to_slave();
        if (ret != 0) {
            ESP_LOGE(TAG, "esp_hosted_connect_to_slave 失败: %d "
                          "(确认从机固件为 ESP-Hosted 3.x SDIO 模式)", ret);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        s_link_up = true;
        ESP_LOGI(TAG, "从机链路已建立");
        notify(WLESS_EVT_LINK_UP, ESP_OK);
        return ESP_OK;
    }

    notify(WLESS_EVT_LINK_FAIL, ESP_FAIL);
    return ESP_FAIL;
}

/* ================ worker 线程 ================ */

static void worker_do_bt_start(void)
{
    if (s_bt_active) {
        ESP_LOGD(TAG, "BT 已启动, 忽略重复请求");
        xEventGroupSetBits(s_events, EV_BT_START_DONE | EV_BT_START_OK);
        return;
    }

    /* 停止请求可能先于启动请求入队后被处理, 重新读取实时意图:
     * 这里按队列顺序执行即可 (请求即意图)。 */

    if (ensure_link_up() != ESP_OK) {
        xEventGroupSetBits(s_events, EV_BT_START_DONE);
        notify(WLESS_EVT_BT_START_FAIL, ESP_FAIL);
        return;
    }

    /* 内存门禁: 不够就先回收, 仍不够则快速失败 */
    if (!ensure_host_ram(true)) {
        xEventGroupSetBits(s_events, EV_BT_START_DONE);
        notify(WLESS_EVT_BT_START_FAIL, ESP_ERR_NO_MEM);
        return;
    }

    ESP_LOGI(TAG, "启动蓝牙协议栈...");
    esp_err_t ret = bsp_bt_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙初始化失败: %s", esp_err_to_name(ret));
        xEventGroupSetBits(s_events, EV_BT_START_DONE);
        notify(WLESS_EVT_BT_START_FAIL, ret);
        return;
    }

    /* 进入可连接/可发现模式, 等待耳机连接 (含自动回连上次设备) */
    bsp_bt_a2dp_source_start();

    s_bt_active = true;
    xEventGroupSetBits(s_events, EV_BT_START_DONE | EV_BT_START_OK);
    notify(WLESS_EVT_BT_STARTED, ESP_OK);
    ESP_LOGI(TAG, "蓝牙已启动");
}

static void worker_do_bt_stop(void)
{
    if (!s_bt_active) {
        xEventGroupSetBits(s_events, EV_BT_START_DONE);
        notify(WLESS_EVT_BT_STOPPED, ESP_OK);
        return;
    }

    /* 1. 若已连接, 先礼貌断开并等待事件落地 (让 audio_player 切回 I2S) */
    if (bsp_bt_is_connected()) {
        ESP_LOGI(TAG, "停止蓝牙: 先断开 A2DP...");
        bsp_bt_disconnect();
        int waited = 0;
        while (bsp_bt_is_connected() && waited < WLESS_BT_DISCONNECT_WAIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(50));
            waited += 50;
        }
        /* 给 audio_player 一点时间从 BT 路径切回 I2S 路径,
         * 避免 PCM ring 被释放时仍被写入 (写侧也有 NULL 保护, 双保险) */
        vTaskDelay(pdMS_TO_TICKS(WLESS_BT_TEARDOWN_GRACE_MS));
    }

    /* 2. 完整反初始化, 回收 Bluedroid 内部 RAM + 256KB PSRAM ring */
    esp_err_t ret = bsp_bt_deinit();
    s_bt_active = false;

    xEventGroupSetBits(s_events, EV_BT_START_DONE);
    notify(WLESS_EVT_BT_STOPPED, ret);
    ESP_LOGI(TAG, "蓝牙已停止 (ret=%s)", esp_err_to_name(ret));
}

static void worker_do_wifi_start(void)
{
    if (s_wifi_active) {
        xEventGroupSetBits(s_events, EV_WIFI_START_DONE | EV_WIFI_START_OK);
        return;
    }

    if (ensure_link_up() != ESP_OK) {
        xEventGroupSetBits(s_events, EV_WIFI_START_DONE);
        notify(WLESS_EVT_WIFI_START_FAIL, ESP_FAIL);
        return;
    }

    /* 内存门禁: 不够就先回收, 仍不够则快速失败 */
    if (!ensure_host_ram(false)) {
        xEventGroupSetBits(s_events, EV_WIFI_START_DONE);
        notify(WLESS_EVT_WIFI_START_FAIL, ESP_ERR_NO_MEM);
        return;
    }

    ESP_LOGI(TAG, "启动 WiFi...");
    esp_err_t ret = bsp_wifi_init();
    if (ret == ESP_OK) {
        s_wifi_active = true;
        xEventGroupSetBits(s_events, EV_WIFI_START_DONE | EV_WIFI_START_OK);
        notify(WLESS_EVT_WIFI_STARTED, ESP_OK);
    } else {
        ESP_LOGE(TAG, "WiFi 初始化失败: %s", esp_err_to_name(ret));
        xEventGroupSetBits(s_events, EV_WIFI_START_DONE);
        notify(WLESS_EVT_WIFI_START_FAIL, ret);
    }
}

static void worker_do_wifi_stop(void)
{
    if (!s_wifi_active) {
        notify(WLESS_EVT_WIFI_STOPPED, ESP_OK);
        return;
    }

    bsp_wifi_disconnect();
    esp_err_t ret = bsp_wifi_deinit();
    s_wifi_active = false;
    notify(WLESS_EVT_WIFI_STOPPED, ret);
    ESP_LOGI(TAG, "WiFi 已停止 (ret=%s)", esp_err_to_name(ret));
}

static void wireless_task_main(void *arg)
{
    (void)arg;
    wless_req_msg_t msg;

    ESP_LOGI(TAG, "无线管理 worker 线程启动");

    for (;;) {
        if (xQueueReceive(s_req_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (msg.type) {
        case WLESS_REQ_BT_START:    worker_do_bt_start();    break;
        case WLESS_REQ_BT_STOP:     worker_do_bt_stop();     break;
        case WLESS_REQ_WIFI_START:  worker_do_wifi_start();  break;
        case WLESS_REQ_WIFI_STOP:   worker_do_wifi_stop();   break;
        default: break;
        }
    }
}

/* ================ 公共接口 ================ */

esp_err_t bsp_wireless_mgr_init(void)
{
    if (s_mgr_initialized) return ESP_OK;

    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    s_req_queue = xQueueCreate(WLESS_QUEUE_LEN, sizeof(wless_req_msg_t));
    if (!s_req_queue) {
        vEventGroupDelete(s_events);
        s_events = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = pdFAIL;
#if (esp_idf_version >= ESP_IDF_VERSION_VAL(5, 2, 0))
    ok = xTaskCreateWithCaps(wireless_task_main, "wless_mgr",
                             WLESS_TASK_STACK_BYTES / sizeof(StackType_t),
                             NULL, WLESS_TASK_PRIO, &s_task,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "PSRAM worker 栈分配失败, 回退内部 RAM");
    }
#endif
    if (ok != pdPASS) {
        ok = xTaskCreate(wireless_task_main, "wless_mgr",
                         WLESS_TASK_STACK_BYTES / sizeof(StackType_t),
                         NULL, WLESS_TASK_PRIO, &s_task);
    }
    if (ok != pdPASS) {
        vQueueDelete(s_req_queue);
        s_req_queue = NULL;
        vEventGroupDelete(s_events);
        s_events = NULL;
        ESP_LOGE(TAG, "worker 线程创建失败");
        return ESP_ERR_NO_MEM;
    }

    /* 注册内存回收钩子: 内部 RAM 紧张时由 bsp_memory 调用 */
    bsp_memory_register_reclaim("wireless", 10, bsp_wireless_reclaim);

    s_mgr_initialized = true;
    ESP_LOGI(TAG, "无线管理器初始化完成 (BT/WiFi 按需启动)");
    return ESP_OK;
}

void bsp_wireless_set_event_cb(wless_event_cb_t cb)
{
    s_event_cb = cb;
}

static esp_err_t post_req(wless_req_type_t type)
{
    if (!s_mgr_initialized) return ESP_ERR_INVALID_STATE;

    wless_req_msg_t msg = { .type = type };
    if (xQueueSend(s_req_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "请求队列已满 (type=%d)", (int)type);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t bsp_wireless_bt_start(void)
{
    xEventGroupClearBits(s_events, EV_BT_START_DONE | EV_BT_START_OK);
    return post_req(WLESS_REQ_BT_START);
}

esp_err_t bsp_wireless_bt_stop(void)
{
    return post_req(WLESS_REQ_BT_STOP);
}

esp_err_t bsp_wireless_wifi_start(void)
{
    xEventGroupClearBits(s_events, EV_WIFI_START_DONE | EV_WIFI_START_OK);
    return post_req(WLESS_REQ_WIFI_START);
}

esp_err_t bsp_wireless_wifi_stop(void)
{
    return post_req(WLESS_REQ_WIFI_STOP);
}

esp_err_t bsp_wireless_bt_ensure_started(uint32_t timeout_ms)
{
    if (!s_mgr_initialized) return ESP_ERR_INVALID_STATE;
    if (s_bt_active) return ESP_OK;

    esp_err_t ret = bsp_wireless_bt_start();
    if (ret != ESP_OK) return ret;

    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           EV_BT_START_DONE | EV_BT_START_OK,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & EV_BT_START_DONE)) {
        return ESP_ERR_TIMEOUT;
    }
    return (bits & EV_BT_START_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_wireless_wifi_ensure_started(uint32_t timeout_ms)
{
    if (!s_mgr_initialized) return ESP_ERR_INVALID_STATE;
    if (s_wifi_active) return ESP_OK;

    esp_err_t ret = bsp_wireless_wifi_start();
    if (ret != ESP_OK) return ret;

    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           EV_WIFI_START_DONE | EV_WIFI_START_OK,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & EV_WIFI_START_DONE)) {
        return ESP_ERR_TIMEOUT;
    }
    return (bits & EV_WIFI_START_OK) ? ESP_OK : ESP_FAIL;
}

bool bsp_wireless_bt_active(void)   { return s_bt_active; }
bool bsp_wireless_wifi_active(void) { return s_wifi_active; }
bool bsp_wireless_link_is_up(void)  { return s_link_up; }

/* ---- 内存回收估算 ----
 * 实测参考 (sdkconfig 相同环境):
 *   Bluedroid + A2DP 栈约占 90-130KB 内部 RAM;
 *   BT PCM ring 256KB (PSRAM);
 *   esp_wifi_remote + lwIP 随流量浮动, 约占 40-60KB 内部 RAM。 */
#define WLESS_BT_INT_RAM_ESTIMATE    (100 * 1024)
#define WLESS_WIFI_INT_RAM_ESTIMATE  (50 * 1024)

uint32_t bsp_wireless_reclaim(uint32_t urgency)
{
    (void)urgency;
    uint32_t est = 0;

    /* 只回收"开着但没在用"的协议栈, 绝不断开使用中的连接;
     * 扫描进行中 (inquiry/active scan 占用控制器) 也不停, 否则用户的
     * 搜索动作会被中断. */
    if (s_bt_active && !bsp_bt_is_connected() &&
        !bsp_bt_is_scanning() && !bsp_bt_classic_is_scanning()) {
        if (post_req(WLESS_REQ_BT_STOP) == ESP_OK) {
            est += WLESS_BT_INT_RAM_ESTIMATE;
            ESP_LOGW(TAG, "低内存: 回收空闲蓝牙栈 (估算 %dKB 内部RAM)",
                     WLESS_BT_INT_RAM_ESTIMATE / 1024);
        }
    }
    if (s_wifi_active && !bsp_wifi_is_connected() && !bsp_wifi_is_scanning()) {
        if (post_req(WLESS_REQ_WIFI_STOP) == ESP_OK) {
            est += WLESS_WIFI_INT_RAM_ESTIMATE;
            ESP_LOGW(TAG, "低内存: 回收空闲 WiFi 栈 (估算 %dKB 内部RAM)",
                     WLESS_WIFI_INT_RAM_ESTIMATE / 1024);
        }
    }
    return est;
}
