/*******************************************************************************
 * PSRAM 内存管理模块 - ESP32-P4
 *
 * 提供:
 *   - PSRAM 状态查询 (是否可用, 容量, 空闲量)
 *   - PSRAM 优先分配封装 (失败时回退到内部 RAM)
 *   - 大块缓冲区分配 (用于音频/LVGL/扫描结果等)
 *   - 内存统计与监控 (供设置页面显示)
 *   - 内存告警回调机制 (低于阈值时通知 UI)
 *   - PSRAM 专属分配器 (C++ std::vector, string 等容器)
 *
 * 设计目标: 保证系统流畅性与稳定性, RAM 都使用 PSRAM 优先,
 *          保留内部 RAM 给中断和 DMA 使用。
 *
 * 优化: 大量使用 PSRAM, 降低内部 RAM 压力
 ******************************************************************************/
#ifndef BSP_MEMORY_H
#define BSP_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 内存类型 (用于监控统计)
 */
typedef enum {
    MEM_TYPE_INTERNAL = 0,    /* 内部 SRAM */
    MEM_TYPE_PSRAM,           /* 外部 PSRAM (SPIRAM) */
    MEM_TYPE_COUNT
} mem_type_t;

/**
 * @brief 内存统计信息
 */
typedef struct {
    uint32_t total;           /* 总容量 (字节) */
    uint32_t free;            /* 当前空闲 (字节) */
    uint32_t min_ever_free;   /* 历史最小空闲 (字节) */
    uint8_t  usage_pct;       /* 使用率百分比 (0-100) */
    uint32_t largest_block;   /* 最大连续空闲块 */
    uint8_t  frag_pct;        /* 碎片度 (0-100): 空闲总量中无法被
                                 最大连续块覆盖的比例, 越高越碎 */
} mem_stats_t;

/**
 * @brief 内存告警回调函数类型
 *
 * 当 PSRAM 空闲低于阈值时被调用 (在监控定时器上下文中)。
 *
 * @param free_bytes 当前空闲字节数
 * @param user_data  用户数据指针
 */
typedef void (*mem_warn_cb_t)(uint32_t free_bytes, void *user_data);

/**
 * @brief 内存分配失败回调函数类型
 *
 * 当 PSRAM 分配失败时调用, 可以在回调中尝试清理内存。
 *
 * @param size      请求的分配大小
 * @param caps      请求的内存 capabilities
 * @param user_data 用户数据指针
 * @return true 释放了一些内存后可以重试
 */
typedef bool (*mem_alloc_fail_cb_t)(size_t size, uint32_t caps, void *user_data);

/**
 * @brief 初始化内存管理模块
 *
 * 检测 PSRAM 可用性, 启动周期性监控任务 (每 5 秒检查一次)。
 * 应在 NVS 初始化后调用。
 *
 * @return ESP_OK 成功, 其他失败
 */
esp_err_t bsp_memory_init(void);

/**
 * @brief 查询 PSRAM 是否可用
 */
bool bsp_memory_psram_is_available(void);

/**
 * @brief 获取 PSRAM 总容量 (字节)
 */
uint32_t bsp_memory_psram_total_size(void);

/**
 * @brief 获取 PSRAM 空闲大小 (字节)
 */
uint32_t bsp_memory_psram_free_size(void);

/**
 * @brief PSRAM 优先分配
 *
 * 优先从 PSRAM 分配, 失败时回退到内部 RAM。
 * 分配的内存可用 free() 释放。
 *
 * @param size 字节数
 * @return 分配的指针, 失败返回 NULL
 */
void *bsp_memory_malloc(size_t size);

/**
 * @brief PSRAM 优先分配 (带 capabilities)
 *
 * @param size  字节数
 * @param caps  heap_caps capabilities (如 MALLOC_CAP_8BIT, MALLOC_CAP_DMA 等)
 * @return 分配的指针, 失败返回 NULL
 */
void *bsp_memory_caps_malloc(size_t size, uint32_t caps);

/**
 * @brief PSRAM 优先分配并清零
 *
 * 等同于 calloc(), 但优先使用 PSRAM。
 *
 * @param nmemb 元素个数
 * @param size  单元素字节数
 * @return 分配的指针, 失败返回 NULL
 */
void *bsp_memory_calloc(size_t nmemb, size_t size);

/**
 * @brief PSRAM 优先分配字符串缓冲区
 *
 * 分配 size 字节的缓冲区, 并在末尾添加 '\0'
 *
 * @param size 包括 '\0' 的总长度
 * @return 分配的指针 (已清零), 失败返回 NULL
 */
char *bsp_memory_strdup(size_t size);

/**
 * @brief PSRAM 优先重新分配
 *
 * @param ptr   原指针 (可以为 NULL)
 * @param size  新大小
 * @return 新指针, 失败返回 NULL (原内存不会被释放!)
 */
void *bsp_memory_realloc(void *ptr, size_t size);

/**
 * @brief PSRAM 优先分配对齐内存
 *
 * @param alignment 对齐要求 (必须是 2 的幂)
 * @param size      字节数
 * @return 分配的指针, 失败返回 NULL
 */
void *bsp_memory_aligned_alloc(size_t alignment, size_t size);

/**
 * @brief 获取指定类型内存的统计信息
 *
 * @param type  内存类型
 * @param stats 输出统计
 * @return ESP_OK 成功
 */
esp_err_t bsp_memory_get_stats(mem_type_t type, mem_stats_t *stats);

/**
 * @brief 注册内存告警回调
 *
 * 当 PSRAM 空闲低于阈值时, 在监控任务上下文中调用回调。
 * 同一时间只允许一个回调, 后注册会覆盖前者。
 *
 * @param cb        回调函数 (NULL 取消注册)
 * @param threshold 阈值 (字节), 默认 64KB
 * @param user_data 透传给回调的用户数据
 */
void bsp_memory_set_warn_cb(mem_warn_cb_t cb, uint32_t threshold,
                            void *user_data);

/**
 * @brief 注册分配失败回调
 *
 * 当 PSRAM 分配失败时调用, 可以在回调中尝试清理缓存等。
 *
 * @param cb        回调函数 (NULL 取消注册)
 * @param caps      触发回调的 capabilities (0 表示所有)
 * @param user_data 透传给回调的用户数据
 */
void bsp_memory_set_alloc_fail_cb(mem_alloc_fail_cb_t cb, uint32_t caps,
                                   void *user_data);

/* ==================== 内存回收钩子 ==================== */

/**
 * @brief 内存回收回调类型
 *
 * 回收方应尽量释放"可再生的"资源 (缓存、空闲的协议栈等),
 * 不要断开正在使用的连接或丢弃用户数据。
 *
 * @param urgency 紧急程度: 0 = 常规回收, 1 = 紧急 (宁可代价大也要腾内存)
 * @return 本次回收预估释放的内部 RAM 字节数
 */
typedef uint32_t (*mem_reclaim_cb_t)(uint32_t urgency);

/**
 * @brief 注册内存回收钩子
 *
 * bsp_memory_trim_caches() 按优先级从小到大依次调用所有钩子。
 * 典型用法: 无线管理器注册高优先级 (10) 钩子, 在内部 RAM 紧张时
 * 停掉"已开启但未连接"的 BT/WiFi 协议栈。
 *
 * @param name      钩子名称 (仅日志用, 可为 NULL)
 * @param priority  优先级 (小者先执行; 0-9 留给轻量缓存类, >=10 为重量级)
 * @param cb        回调函数
 * @return ESP_OK 成功, ESP_ERR_NO_MEM 槽位满 (最多 8 个), ESP_ERR_INVALID_ARG 无回调
 */
esp_err_t bsp_memory_register_reclaim(const char *name, int priority,
                                      mem_reclaim_cb_t cb);

/**
 * @brief 设置内部 RAM 低水位阈值
 *
 * 低于阈值时监控任务会触发一次全量回收 (bsp_memory_trim_caches(1)),
 * 带 60s 冷却时间防止反复启停协议栈。默认 40KB, 传 0 关闭。
 *
 * @param bytes 阈值 (字节)
 */
void bsp_memory_set_internal_warn_threshold(uint32_t bytes);

/**
 * @brief 触发一次内存回收 (调用 heap_caps_malloc 触发碎片整理)
 *
 * @return 回收的内存量 (字节, 仅作日志参考)
 */
uint32_t bsp_memory_defrag(void);

/**
 * @brief 触发一次全量内存回收
 *
 * 先做碎片整理, 再按优先级从小到大调用所有已注册的回收钩子
 * (bsp_memory_register_reclaim)。内部 RAM 低于低水位阈值时,
 * 监控任务会自动以 urgency=1 调用 (带 60s 冷却)。
 *
 * @param urgency 紧急程度: 0 = 常规, 1 = 紧急 (透传给各钩子)
 * @return 碎片整理实测回收 + 各钩子预估回收的字节总数 (钩子回收异步完成)
 */
uint32_t bsp_memory_trim_caches(uint32_t urgency);

/**
 * @brief 格式化内存统计为字符串 (供UI显示)
 *
 * 格式: "PSRAM: 12.3MB / 16.0MB (78%)"
 *
 * @param buf   输出缓冲区
 * @param len   缓冲区长度
 * @param type  内存类型
 */
void bsp_memory_format_stats(char *buf, size_t len, mem_type_t type);

/**
 * @brief 获取详细的内存信息 (用于调试)
 *
 * @param buf   输出缓冲区
 * @param len   缓冲区长度
 */
void bsp_memory_dump_info(char *buf, size_t len);

#ifdef __cplusplus
}

/* ============================================================================
 * C++ 内存分配器封装 (用于 std::vector, std::string 等容器)
 * ============================================================================
 * 使用方法:
 *   #include "bsp_memory.h"
 *   std::vector<int, PSRAMAllocator<int>> my_vec;
 *   using PSRAMString = std::basic_string<char, std::char_traits<char>, PSRAMStringAllocator<char>>;
 */

#include <cstdio>
#include <exception>
#include <new>
#include <string>
#include <type_traits>

/**
 * @brief PSRAM 优先的 C++ 内存分配器
 *
 * 用于 std::vector, std::map 等 STL 容器:
 *   std::vector<int, PSRAMAllocator<int>> vec;
 *
 * @tparam T 元素类型
 */
template<typename T>
class PSRAMAllocator {
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;

    PSRAMAllocator() noexcept = default;

    template<typename U>
    PSRAMAllocator(const PSRAMAllocator<U>&) noexcept {}

    T* allocate(size_type n) {
        if (n == 0) return nullptr;
        size_t bytes = n * sizeof(T);
        void* p = bsp_memory_malloc(bytes);
        if (p == nullptr) {
            fprintf(stderr, "PSRAMAllocator: allocate failed: %zu bytes\n", bytes);
            throw std::bad_alloc();
        }
        return static_cast<T*>(p);
    }

    void deallocate(T* p, size_type) noexcept {
        free(p);
    }

    template<typename U>
    bool operator==(const PSRAMAllocator<U>&) const noexcept { return true; }

    template<typename U>
    bool operator!=(const PSRAMAllocator<U>&) const noexcept { return false; }
};

/**
 * @brief PSRAM 优先的字符串分配器
 *
 * 用于 std::string:
 *   using PSRAMString = std::basic_string<char, std::char_traits<char>, PSRAMStringAllocator<char>>;
 *   PSRAMString str;
 */
template<typename T>
class PSRAMStringAllocator {
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;

    PSRAMStringAllocator() noexcept = default;

    template<typename U>
    PSRAMStringAllocator(const PSRAMStringAllocator<U>&) noexcept {}

    T* allocate(size_type n) {
        if (n == 0) return nullptr;
        size_t bytes = n * sizeof(T);
        void* p = bsp_memory_malloc(bytes);
        if (p == nullptr) {
            fprintf(stderr, "PSRAMStringAllocator: allocate failed: %zu bytes\n", bytes);
            throw std::bad_alloc();
        }
        return static_cast<T*>(p);
    }

    void deallocate(T* p, size_type) noexcept {
        free(p);
    }

    template<typename U>
    bool operator==(const PSRAMStringAllocator<U>&) const noexcept { return true; }

    template<typename U>
    bool operator!=(const PSRAMStringAllocator<U>&) const noexcept { return false; }
};

/* 便捷类型别名 */
using PSRAMString = std::basic_string<char, std::char_traits<char>, PSRAMStringAllocator<char>>;

#endif /* __cplusplus */

#endif /* BSP_MEMORY_H */
