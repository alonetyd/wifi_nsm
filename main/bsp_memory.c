/*******************************************************************************
 * PSRAM 内存管理模块实现 - ESP32-P4
 ******************************************************************************/
#include "bsp_memory.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "bsp_memory";
#define MEM_MONITOR_PERIOD_MS    5000
#define MEM_WARN_THRESHOLD_DEFAULT (64 * 1024)

/* ================ PSRAM-DMA 堆分区 ================ */
/* IDF 5.5.4 注册 PSRAM 堆时不带 MALLOC_CAP_DMA, 导致
 * heap_caps_*(MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA) 永远返回 NULL,
 * esp-hosted 的 PSRAM-DMA 路径因此失效, SDIO 收发缓冲全部压在内部 RAM.
 * ESP32-P4 的 GDMA 可以访问 PSRAM (SOC_PSRAM_DMA_CAPABLE=1),
 * SDMMC 驱动也支持 cache 行对齐的 PSRAM 缓冲.
 * 这里从 PSRAM 堆切出一块重新注册为带 DMA 能力的堆区,
 * 让 esp-hosted 的 SDIO 缓冲迁到 PSRAM, 释放内部 RAM 给蓝牙栈. */
#define PSRAM_DMA_HEAP_SIZE (1 * 1024 * 1024)

static bool s_psram_available = false;   /* 前置引用, 实际定义在下方状态区 */

static void psram_dma_heap_init(void)
{
    if (!s_psram_available) return;

    void *mem = heap_caps_aligned_alloc(64, PSRAM_DMA_HEAP_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) {
        ESP_LOGW(TAG, "PSRAM-DMA 划区失败: PSRAM 空闲不足 %dKB",
                 PSRAM_DMA_HEAP_SIZE / 1024);
        return;
    }

    /* caps 三层是"优先级匹配" (heap_caps_base.c 从 prio0 逐层向下找):
     * DMA 绝不能放 prio0! 本堆通过 heap_caps_add_region_with_caps 插在
     * 堆链表头部, 若 caps[0] 带 DMA, 所有普通 MALLOC_CAP_DMA 请求
     * (SDMMC 驱动的 SCR/CSD/CID 等 8~64B 小命令缓冲) 会最先命中本
     * PSRAM 堆; 而 PSRAM 缓冲须满足 L2 cache 行 (128B) 地址+长度对齐,
     * 小缓冲过不了 sdmmc_host_check_buffer_alignment, SD 卡初始化
     * 报 0x102 (ESP_ERR_INVALID_ARG) 挂载失败.
     * DMA 放 prio1 后:
     *   - 普通 DMA 请求在 prio0 即被内部 RAM (其 caps[0] 带 DMA) 命中;
     *   - SPIRAM|DMA 组合 (esp-hosted SDIO 缓冲/DPI 帧缓冲) prio0 只有
     *     本堆能同时满足两个 cap 位, 落到 prio1 命中本堆, PSRAM-DMA
     *     路径不受影响 (esp_heap_adjust_alignment_to_hw 对带 SPIRAM 的
     *     DMA 请求会剥掉 DMA 位按 SPIRAM|8BIT 匹配, 本堆 caps[0] 的
     *     SPIRAM 位照样优先命中). */
    const uint32_t caps[] = {
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT,
        MALLOC_CAP_DMA,
        MALLOC_CAP_8BIT | MALLOC_CAP_32BIT,
    };
    esp_err_t err = heap_caps_add_region_with_caps(caps,
            (intptr_t)mem, (intptr_t)mem + PSRAM_DMA_HEAP_SIZE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PSRAM-DMA 堆区注册失败: %s", esp_err_to_name(err));
        heap_caps_free(mem);
        return;
    }
    ESP_LOGI(TAG, "PSRAM-DMA 堆区就绪: %dKB @ %p (DMA 缓冲优先放这里)",
             PSRAM_DMA_HEAP_SIZE / 1024, mem);
}

/* ================ 内存回收钩子 ================ */
#define MEM_RECLAIM_MAX_SLOTS    8
#define MEM_RECLAIM_COOLDOWN_MS  60000   /* 低水位回收冷却时间 */
#define MEM_INTERNAL_WARN_DEFAULT (40 * 1024)  /* 内部 RAM 低水位默认值 */

typedef struct {
    const char      *name;
    int              priority;   /* 小者先执行 */
    mem_reclaim_cb_t cb;
} mem_reclaim_slot_t;

static mem_reclaim_slot_t s_reclaim_slots[MEM_RECLAIM_MAX_SLOTS];
static int                s_reclaim_count = 0;
static uint32_t           s_internal_warn_threshold = MEM_INTERNAL_WARN_DEFAULT;
static TickType_t         s_last_reclaim_tick = 0;

/* 内存统计 */
typedef struct {
    uint32_t total;
    uint32_t free;
    uint32_t min_ever_free;
    uint32_t largest_block;
} mem_info_t;

static bool              s_initialized = false;
/* s_psram_available 定义在文件头部 (psram_dma_heap_init 前置引用) */
static mem_warn_cb_t     s_warn_cb = NULL;
static uint32_t          s_warn_threshold = MEM_WARN_THRESHOLD_DEFAULT;
static void             *s_warn_user_data = NULL;
static mem_alloc_fail_cb_t s_alloc_fail_cb = NULL;
static uint32_t          s_alloc_fail_caps = 0;
static void             *s_alloc_fail_user_data = NULL;
static TimerHandle_t     s_monitor_timer = NULL;
static mem_info_t        s_mem_info[MEM_TYPE_COUNT] = {0};

/* 内部辅助函数 */
static inline uint32_t heap_total(mem_type_t type)
{
    if (type == MEM_TYPE_PSRAM)
        return heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    return heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
}

static inline uint32_t heap_free(mem_type_t type)
{
    if (type == MEM_TYPE_PSRAM)
        return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

static inline uint32_t heap_largest_block(mem_type_t type)
{
    if (type == MEM_TYPE_PSRAM)
        return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void update_mem_info(mem_type_t type)
{
    uint32_t free_now = heap_free(type);
    uint32_t largest = heap_largest_block(type);

    s_mem_info[type].free = free_now;
    s_mem_info[type].largest_block = largest;

    if (s_mem_info[type].min_ever_free == 0 || free_now < s_mem_info[type].min_ever_free) {
        s_mem_info[type].min_ever_free = free_now;
    }
}

static void monitor_cb(TimerHandle_t timer)
{
    (void)timer;

    for (int t = 0; t < MEM_TYPE_COUNT; t++) {
        update_mem_info((mem_type_t)t);
    }

    /* PSRAM 告警检查 */
    if (s_psram_available && s_warn_cb) {
        uint32_t psram_free = s_mem_info[MEM_TYPE_PSRAM].free;
        if (psram_free < s_warn_threshold) {
            ESP_LOGW(TAG, "PSRAM 空闲不足: %lu bytes (阈值 %lu)",
                     (unsigned long)psram_free, (unsigned long)s_warn_threshold);
            s_warn_cb(psram_free, s_warn_user_data);
        }
    }

    /* 内部 RAM 低水位检查: 触发一次紧急回收 (带冷却时间,
     * 防止反复启停 BT/WiFi 协议栈) */
    if (s_internal_warn_threshold > 0) {
        uint32_t int_free = s_mem_info[MEM_TYPE_INTERNAL].free;
        if (int_free < s_internal_warn_threshold) {
            TickType_t now = xTaskGetTickCount();
            TickType_t cooldown = pdMS_TO_TICKS(MEM_RECLAIM_COOLDOWN_MS);
            if (s_last_reclaim_tick == 0 || (now - s_last_reclaim_tick) > cooldown) {
                s_last_reclaim_tick = now;
                ESP_LOGW(TAG, "内部RAM低于低水位: %lu bytes (阈值 %lu), "
                         "内部DMA最大连续块 %lu bytes, 触发紧急回收",
                         (unsigned long)int_free,
                         (unsigned long)s_internal_warn_threshold,
                         (unsigned long)heap_caps_get_largest_free_block(
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
                bsp_memory_trim_caches(1);
            }
        }
    }

    ESP_LOGD(TAG, "内部RAM: %lu/%lu (最大块:%lu) PSRAM: %lu/%lu (最大块:%lu)",
             (unsigned long)s_mem_info[MEM_TYPE_INTERNAL].free,
             (unsigned long)s_mem_info[MEM_TYPE_INTERNAL].total,
             (unsigned long)s_mem_info[MEM_TYPE_INTERNAL].largest_block,
             (unsigned long)s_mem_info[MEM_TYPE_PSRAM].free,
             (unsigned long)s_mem_info[MEM_TYPE_PSRAM].total,
             (unsigned long)s_mem_info[MEM_TYPE_PSRAM].largest_block);
}

esp_err_t bsp_memory_init(void)
{
    if (s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "PSRAM 内存管理模块初始化");
    ESP_LOGI(TAG, "========================================");

    /* 检测 PSRAM */
    s_psram_available = (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0);

    if (s_psram_available) {
        uint32_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        uint32_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        ESP_LOGI(TAG, "PSRAM 可用!");
        ESP_LOGI(TAG, "  总容量:   %lu KB (%.1f MB)",
                 (unsigned long)(total / 1024), (double)total / (1024.0 * 1024.0));
        ESP_LOGI(TAG, "  空闲:     %lu KB (%.1f MB)",
                 (unsigned long)(free / 1024), (double)free / (1024.0 * 1024.0));
        ESP_LOGI(TAG, "  最大块:   %lu KB",
                 (unsigned long)(largest / 1024));

        s_mem_info[MEM_TYPE_PSRAM].total = total;
        s_mem_info[MEM_TYPE_PSRAM].free = free;
        s_mem_info[MEM_TYPE_PSRAM].min_ever_free = free;
        s_mem_info[MEM_TYPE_PSRAM].largest_block = largest;

        /* 从 PSRAM 切一块注册为 DMA-capable 堆区 (给 esp-hosted 用) */
        psram_dma_heap_init();
    } else {
        ESP_LOGW(TAG, "PSRAM 不可用, 仅使用内部 RAM!");
    }

    /* 内部 RAM 统计 */
    uint32_t int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    uint32_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "内部 RAM:");
    ESP_LOGI(TAG, "  总容量:   %lu KB",
             (unsigned long)(int_total / 1024));
    ESP_LOGI(TAG, "  空闲:     %lu KB",
             (unsigned long)(int_free / 1024));
    ESP_LOGI(TAG, "  最大块:   %lu KB",
             (unsigned long)(int_largest / 1024));

    s_mem_info[MEM_TYPE_INTERNAL].total = int_total;
    s_mem_info[MEM_TYPE_INTERNAL].free = int_free;
    s_mem_info[MEM_TYPE_INTERNAL].min_ever_free = int_free;
    s_mem_info[MEM_TYPE_INTERNAL].largest_block = int_largest;

    /* 启动监控定时器 */
    s_monitor_timer = xTimerCreate("mem_monitor",
                                   pdMS_TO_TICKS(MEM_MONITOR_PERIOD_MS),
                                   pdTRUE, NULL, monitor_cb);
    if (s_monitor_timer) {
        xTimerStart(s_monitor_timer, 0);
        ESP_LOGI(TAG, "内存监控定时器已启动 (周期 %d ms)", MEM_MONITOR_PERIOD_MS);
    } else {
        ESP_LOGW(TAG, "定时器创建失败");
    }

    s_initialized = true;
    return ESP_OK;
}

bool bsp_memory_psram_is_available(void)
{
    return s_psram_available;
}

uint32_t bsp_memory_psram_total_size(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
}

uint32_t bsp_memory_psram_free_size(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

void *bsp_memory_malloc(size_t size)
{
    void *p = NULL;

    if (size == 0) {
        return NULL;
    }

    /* 优先尝试 PSRAM */
    if (s_psram_available) {
        p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    /* PSRAM 失败, 尝试内部 RAM */
    if (p == NULL) {
        p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    /* 最后的手段: 标准 malloc */
    if (p == NULL) {
        p = malloc(size);
    }

    if (p == NULL) {
        ESP_LOGE(TAG, "所有分配方式失败: size:%u", (unsigned)size);

        /* 调用失败回调, 回调中可尝试清理内存; 返回 true 则重试一次 */
        if (s_alloc_fail_cb) {
            bool recovered = s_alloc_fail_cb(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                            s_alloc_fail_user_data);
            if (recovered) {
                p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (p == NULL) {
                    p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                }
                if (p) {
                    ESP_LOGI(TAG, "分配失败回调清理内存后重试成功");
                }
            }
        }

        /* 最后防线: 触发一次紧急回收后再试 */
        if (p == NULL) {
            bsp_memory_trim_caches(1);
            p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (p == NULL) {
                p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
            if (p) {
                ESP_LOGI(TAG, "紧急回收后分配成功: size:%u", (unsigned)size);
            }
        }
    }

    return p;
}

void *bsp_memory_caps_malloc(size_t size, uint32_t caps)
{
    void *p = NULL;

    if (size == 0) {
        return NULL;
    }

    /* 如果请求不包含 PSRAM 标志且 PSRAM 可用, 添加 PSRAM */
    uint32_t alloc_caps = caps;
    if (!(caps & MALLOC_CAP_SPIRAM) && (caps & MALLOC_CAP_INTERNAL) == 0) {
        /* 既没有 PSRAM 也没有 INTERNAL 标志, 默认尝试 PSRAM */
        if (s_psram_available) {
            alloc_caps |= MALLOC_CAP_SPIRAM;
        }
    }

    /* 优先尝试 PSRAM */
    if (s_psram_available && (alloc_caps & MALLOC_CAP_SPIRAM)) {
        p = heap_caps_malloc(size, alloc_caps);
    }

    /* 回退到内部 RAM */
    if (p == NULL && (alloc_caps & MALLOC_CAP_INTERNAL)) {
        p = heap_caps_malloc(size, alloc_caps & ~MALLOC_CAP_SPIRAM);
    }

    /* 最后尝试标准 malloc */
    if (p == NULL) {
        p = malloc(size);
    }

    if (p == NULL) {
        ESP_LOGE(TAG, "caps_malloc 失败: size:%u caps:0x%lx", (unsigned)size, (unsigned long)caps);

        if (s_alloc_fail_cb) {
            s_alloc_fail_cb(size, caps, s_alloc_fail_user_data);
        }
    }

    return p;
}

void *bsp_memory_calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    if (total == 0) {
        return NULL;
    }

    void *p = bsp_memory_malloc(total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

char *bsp_memory_strdup(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    char *p = (char *)bsp_memory_malloc(size);
    if (p) {
        p[0] = '\0';
    }
    return p;
}

void *bsp_memory_realloc(void *ptr, size_t size)
{
    if (size == 0) {
        if (ptr) free(ptr);
        return NULL;
    }
    if (ptr == NULL) {
        return bsp_memory_malloc(size);
    }

    /* 注意: heap_caps_get_allocated_size() 返回的是"已分配大小"而非 caps,
     * 不能当作 caps 传给 heap_caps_realloc. 这里按指针所在的物理内存
     * 重新推导 caps, 保证在原堆上原地扩展. */
    size_t old_size = heap_caps_get_allocated_size(ptr);
    uint32_t caps = esp_ptr_external_ram(ptr) ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                              : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void *new_ptr = heap_caps_realloc(ptr, size, caps);
    if (new_ptr) {
        return new_ptr;
    }

    /* 原地扩展失败: 分配新块并复制 (PSRAM 优先) */
    new_ptr = bsp_memory_malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, (old_size < size) ? old_size : size);
        free(ptr);
    }

    return new_ptr;
}

void *bsp_memory_aligned_alloc(size_t alignment, size_t size)
{
    void *p = NULL;

    if (s_psram_available) {
        p = heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    if (p == NULL) {
        p = heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (p == NULL) {
        ESP_LOGE(TAG, "aligned_alloc 失败: alignment:%u size:%u",
                 (unsigned)alignment, (unsigned)size);
    }

    return p;
}

esp_err_t bsp_memory_get_stats(mem_type_t type, mem_stats_t *stats)
{
    if (!stats || type >= MEM_TYPE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    stats->total = s_mem_info[type].total;
    stats->free = s_mem_info[type].free;
    stats->min_ever_free = s_mem_info[type].min_ever_free;
    stats->largest_block = s_mem_info[type].largest_block;
    stats->usage_pct = stats->total ? (uint8_t)((stats->total - stats->free) * 100 / stats->total) : 0;

    /* 碎片度: 空闲总量中无法被最大连续块覆盖的比例 */
    stats->frag_pct = 0;
    if (stats->free > 0 && stats->largest_block < stats->free) {
        stats->frag_pct = (uint8_t)((stats->free - stats->largest_block) * 100 / stats->free);
    }

    return ESP_OK;
}

void bsp_memory_set_warn_cb(mem_warn_cb_t cb, uint32_t threshold, void *user_data)
{
    s_warn_cb = cb;
    s_warn_threshold = threshold;
    s_warn_user_data = user_data;
    ESP_LOGI(TAG, "告警阈值设置为 %lu bytes", (unsigned long)threshold);
}

void bsp_memory_set_alloc_fail_cb(mem_alloc_fail_cb_t cb, uint32_t caps, void *user_data)
{
    s_alloc_fail_cb = cb;
    s_alloc_fail_caps = caps;
    s_alloc_fail_user_data = user_data;
    ESP_LOGI(TAG, "分配失败回调已注册");
}

uint32_t bsp_memory_defrag(void)
{
    /* IDF 的 multi-heap 不支持运行时堆整理 (已分配块无法搬移),
     * 旧版 "alloc(1)+free 触发碎片整理" 是安慰剂, 不会有任何效果。
     * 真正的碎片缓解手段:
     *   - 大缓冲优先 PSRAM (bsp_memory_malloc 已做);
     *   - 生命周期相同的对象一起分配/释放, 减少交错;
     *   - 内部 RAM 不足时通过回收钩子 (bsp_memory_trim_caches) 停掉
     *     空闲协议栈, 释放整块连续内存。
     * 保留 API 是为了兼容既有调用点, 恒返回 0。 */
    return 0;
}

/* ================ 内存回收钩子实现 ================ */

esp_err_t bsp_memory_register_reclaim(const char *name, int priority,
                                      mem_reclaim_cb_t cb)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_reclaim_count >= MEM_RECLAIM_MAX_SLOTS) {
        ESP_LOGE(TAG, "回收钩子槽位已满 (%d)", MEM_RECLAIM_MAX_SLOTS);
        return ESP_ERR_NO_MEM;
    }

    /* 按优先级从小到大插入 (同优先级后注册者靠后) */
    int pos = s_reclaim_count;
    for (int i = 0; i < s_reclaim_count; i++) {
        if (priority < s_reclaim_slots[i].priority) {
            pos = i;
            break;
        }
    }
    for (int i = s_reclaim_count; i > pos; i--) {
        s_reclaim_slots[i] = s_reclaim_slots[i - 1];
    }
    s_reclaim_slots[pos].name = name;
    s_reclaim_slots[pos].priority = priority;
    s_reclaim_slots[pos].cb = cb;
    s_reclaim_count++;

    ESP_LOGI(TAG, "已注册内存回收钩子: %s (priority=%d, 共%d个)",
             name ? name : "(anon)", priority, s_reclaim_count);
    return ESP_OK;
}

void bsp_memory_set_internal_warn_threshold(uint32_t bytes)
{
    s_internal_warn_threshold = bytes;
    ESP_LOGI(TAG, "内部RAM低水位阈值: %lu bytes %s",
             (unsigned long)bytes, bytes == 0 ? "(关闭)" : "");
}

uint32_t bsp_memory_trim_caches(uint32_t urgency)
{
    uint32_t total_recovered = 0;

    /* 1. 触发碎片整理 */
    total_recovered += bsp_memory_defrag();

    /* 2. 按优先级从小到大调用所有已注册的回收钩子
     *    (0-9 轻量缓存类, >=10 重量级如无线协议栈启停) */
    for (int i = 0; i < s_reclaim_count; i++) {
        if (s_reclaim_slots[i].cb == NULL) continue;
        uint32_t got = s_reclaim_slots[i].cb(urgency);
        ESP_LOGI(TAG, "回收钩子 [%s] (prio %d) 预估回收 %lu bytes",
                 s_reclaim_slots[i].name ? s_reclaim_slots[i].name : "(anon)",
                 s_reclaim_slots[i].priority, (unsigned long)got);
        total_recovered += got;
    }

    ESP_LOGI(urgency ? "bsp_memory" : TAG,
             "内存回收完成 (urgency=%lu), 预估共回收: %lu bytes",
             (unsigned long)urgency, (unsigned long)total_recovered);

    return total_recovered;
}

void bsp_memory_format_stats(char *buf, size_t len, mem_type_t type)
{
    if (!buf || len == 0) return;

    mem_stats_t st = {0};
    if (bsp_memory_get_stats(type, &st) != ESP_OK) {
        snprintf(buf, len, "%s: 未知", (type == MEM_TYPE_PSRAM) ? "PSRAM" : "内部RAM");
        return;
    }

    const char *name = (type == MEM_TYPE_PSRAM) ? "PSRAM" : "内部RAM";

    if (st.total >= 1024 * 1024) {
        snprintf(buf, len, "%s: %.1fMB / %.1fMB (%u%%)",
                 name,
                 (double)st.free / (1024.0 * 1024.0),
                 (double)st.total / (1024.0 * 1024.0),
                 st.usage_pct);
    } else {
        snprintf(buf, len, "%s: %luKB / %luKB (%u%%)",
                 name,
                 (unsigned long)(st.free / 1024),
                 (unsigned long)(st.total / 1024),
                 st.usage_pct);
    }
}

void bsp_memory_dump_info(char *buf, size_t len)
{
    if (!buf || len == 0) return;

    int pos = 0;

    if (s_psram_available) {
        mem_stats_t st = {0};
        if (bsp_memory_get_stats(MEM_TYPE_PSRAM, &st) == ESP_OK) {
            pos += snprintf(buf + pos, len - pos,
                           "PSRAM:\n"
                           "  Total:    %.1f MB\n"
                           "  Used:     %.1f MB (%u%%)\n"
                           "  Free:     %.1f MB\n"
                           "  MinEver:  %.1f MB\n"
                           "  Largest:  %.1f MB\n",
                           (double)st.total / (1024.0 * 1024.0),
                           (double)(st.total - st.free) / (1024.0 * 1024.0),
                           st.usage_pct,
                           (double)st.free / (1024.0 * 1024.0),
                           (double)st.min_ever_free / (1024.0 * 1024.0),
                           (double)st.largest_block / (1024.0 * 1024.0));
        }
    }

    mem_stats_t st_int = {0};
    bsp_memory_get_stats(MEM_TYPE_INTERNAL, &st_int);

    pos += snprintf(buf + pos, len - pos,
                   "Internal RAM:\n"
                   "  Total:    %lu KB\n"
                   "  Used:     %lu KB (%u%%)\n"
                   "  Free:     %lu KB\n"
                   "  MinEver:  %lu KB\n"
                   "  Largest:  %lu KB",
                   (unsigned long)(st_int.total / 1024),
                   (unsigned long)((st_int.total - st_int.free) / 1024),
                   st_int.usage_pct,
                   (unsigned long)(st_int.free / 1024),
                   (unsigned long)(st_int.min_ever_free / 1024),
                   (unsigned long)(st_int.largest_block / 1024));
}
