/**
 * @file lv_mem_psram.c
 *
 * LVGL 自定义内存分配器 (ESP32-P4)
 *
 * 将 LVGL 的全部内存 (UI 页面/控件/样式/字体引用/图片缓存等)
 * 优先分配到 PSRAM, 仅在 PSRAM 分配失败时回退内部 RAM。
 * 配合 sdkconfig 中的 CONFIG_LV_USE_CUSTOM_MALLOC=y 使用,
 * LVGL 会调用本文件实现的 lv_*_core() 系列函数。
 *
 * 收益: 原 LV_USE_BUILTIN_MALLOC 使用 128KB 静态池 (内部 RAM),
 * 切换后该池被移除, 为蓝牙/WiFi 协议栈腾出内部 RAM。
 */
#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "lv_mem";

/* LVGL 内存统一走 PSRAM (P4 的 PSRAM 为 EDMA 能力, 200MHz, 性能足够 UI 使用) */
#define LV_PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
    ESP_LOGI(TAG, "LVGL 使用 PSRAM 自定义分配器 (PSRAM 空闲 %u KB)",
             (unsigned)(heap_caps_get_free_size(LV_PSRAM_CAPS) / 1024));
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    /* 不支持追加内存池 */
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, LV_PSRAM_CAPS);
    if (p == NULL && size > 0) {
        /* PSRAM 不足时回退内部 RAM, 优先保证 UI 不崩溃 */
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
        if (p) {
            ESP_LOGW(TAG, "PSRAM 分配 %u 字节失败, 回退内部 RAM", (unsigned)size);
        }
    }
    return p;
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, LV_PSRAM_CAPS);
    if (n == NULL && new_size > 0) {
        n = heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
        if (n) {
            ESP_LOGW(TAG, "PSRAM realloc %u 字节失败, 回退内部 RAM", (unsigned)new_size);
        }
    }
    return n;
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    if (mon_p == NULL) {
        return;
    }
    memset(mon_p, 0, sizeof(*mon_p));

    /* 上报 PSRAM 整体堆状况, 供 UI/日志观察内存趋势 */
    multi_heap_info_t info;
    heap_caps_get_info(&info, LV_PSRAM_CAPS);

    size_t total = info.total_free_bytes + info.total_allocated_bytes;
    mon_p->total_size = total;
    mon_p->free_size = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->used_cnt = info.allocated_blocks;
    mon_p->max_used = total - info.minimum_free_bytes;
    mon_p->used_pct = total ? (uint8_t)((info.total_allocated_bytes * 100) / total) : 0;
    mon_p->frag_pct = mon_p->free_size ?
                      (uint8_t)(100 - ((uint64_t)info.largest_free_block * 100) / mon_p->free_size) : 0;
}

lv_result_t lv_mem_test_core(void)
{
    /* 堆完整性由 IDF heap 组件管理, 这里返回 OK */
    return LV_RESULT_OK;
}

#endif /* LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM */
