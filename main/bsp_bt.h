/**
 * @file bsp_bt.h
 * @brief 蓝牙模块 - 通过ESP-Hosted SDIO使用经典蓝牙A2DP + BLE 4.2
 *
 * ESP32-P4本身没有蓝牙硬件，通过ESP-Hosted组件经SDIO与协处理器
 * (ESP32)通信。ESP32协处理器支持经典蓝牙(A2DP)和BLE 4.2。
 * 注意: 不支持BLE 5.0 (ESP32硬件限制)。
 *
 * @note 使用前需在menuconfig中启用:
 *       - CONFIG_BT_ENABLED=y
 *       - CONFIG_BT_BLUEDROID_ENABLED=y
 *       - CONFIG_BT_CLASSIC_ENABLED=y
 *       - CONFIG_BT_A2DP_ENABLE=y
 *       - CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y (禁用BLE 5.0)
 *
 * 已配对设备持久化:
 *   使用 NVS 命名空间 "bt_paired" 保存已配对的蓝牙设备地址和名称,
 *   断电后保留，重启后可在 UI 中独立展示。
 */

#ifndef BSP_BT_H
#define BSP_BT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BLE扫描设备信息 */
typedef struct {
    char addr[18];          /* 设备地址 "xx:xx:xx:xx:xx:xx" */
    char name[64];          /* 设备名称 */
    int8_t rssi;            /* 信号强度 */
    uint8_t dev_type;       /* 设备类型: 0=未知, 1=经典BT, 2=BLE, 3=双模 */
    uint32_t cod;           /* Class of Device (经典BT) - 用于识别耳机/扬声器 */
    bool is_audio_device;   /* 是否为音频设备 (A2DP sink, 如蓝牙耳机) */
} ble_device_t;

/* 已配对设备信息 (持久化在NVS中) */
typedef struct {
    char addr[18];          /* 设备地址 */
    char name[64];          /* 设备名称 */
} bt_paired_dev_t;

/* BLE扫描回调: 每发现一个设备调用一次 */
typedef void (*ble_scan_cb_t)(const ble_device_t *dev);

/* 经典蓝牙扫描回调 (用于发现A2DP耳机/扬声器) */
typedef ble_scan_cb_t bt_classic_scan_cb_t;

/* BT连接事件回调 */
typedef void (*bt_event_cb_t)(bool connected, const char *device_name);

/**
 * @brief 初始化蓝牙
 *
 * 初始化BT控制器(通过ESP-Hosted)、Bluedroid协议栈、A2DP源。
 * SDIO引脚由ESP-Hosted组件通过menuconfig配置管理。
 *
 * 成功后若存在"上次连接设备" (NVS) 且自动回连开启, 会通过工作线程
 * 延迟发起回连, UI 无需再手动 bsp_bt_connect()。
 *
 * @return ESP_OK 成功，ESP_ERR_NOT_SUPPORTED表示未启用蓝牙
 */
esp_err_t bsp_bt_init(void);
bool bsp_bt_is_initialized(void);

/**
 * @brief 完整反初始化蓝牙并回收内存 (给 bsp_wireless_mgr 按需启停用)
 *
 * 执行: 停扫描/停重连定时器 -> (若已连接)断开A2DP并等待 ->
 * esp_a2d_source_deinit -> esp_bluedroid_disable/deinit ->
 * esp_hosted_hci_bluedroid_teardown -> 释放 256KB PCM ring (PSRAM)。
 *
 * 已配对列表与"上次连接设备"记录保留在 NVS, 下次 bsp_bt_init 后可回连。
 * 必须与 bsp_bt_init() 成对调用; 调用后所有 bsp_bt_* 接口返回
 * ESP_ERR_INVALID_STATE, 直到重新 init。
 *
 * @return ESP_OK 成功
 */
esp_err_t bsp_bt_deinit(void);

/**
 * @brief 开始A2DP源 (向蓝牙耳机发送音频)
 *
 * 设置设备为可连接、可发现模式，等待蓝牙耳机连接。
 *
 * @return ESP_OK 成功，ESP_ERR_NOT_SUPPORTED表示未启用蓝牙
 */
esp_err_t bsp_bt_a2dp_source_start(void);

/**
 * @brief 请求配对并连接指定设备
 *
 * 1. 启动SSP配对流程 (如果尚未配对)
 * 2. 配对成功后写入 NVS "bt_paired" 命名空间
 * 3. 自动发起 A2DP 连接
 *
 * UI 端会先弹出"确认配对"对话框，用户点确认后才调用本接口。
 *
 * @param addr 目标设备地址，格式为 "xx:xx:xx:xx:xx:xx"
 * @param name 设备名称 (可选, 用于保存到NVS, 可为NULL)
 * @return ESP_OK 发起成功
 */
esp_err_t bsp_bt_pair_and_connect(const char *addr, const char *name);

/**
 * @brief 连接到蓝牙设备
 *
 * 仅在已经配对过的情况下使用。如果设备未配对，建议先调用
 * bsp_bt_pair_and_connect()。
 *
 * @param addr 目标设备地址，格式为 "xx:xx:xx:xx:xx:xx"
 * @return ESP_OK 成功，ESP_ERR_NOT_SUPPORTED表示未启用蓝牙
 */
esp_err_t bsp_bt_connect(const char *addr);

/**
 * @brief 断开蓝牙
 *
 * 真实地发起 A2DP / ACL 断开请求, 同时清本地状态。返回的 ESP_OK 表示
 * 断开请求已发起, 实际的 connection state changed 事件会通过 s_event_cb 通知。
 */
esp_err_t bsp_bt_disconnect(void);

/**
 * @brief 蓝牙连接状态
 * @return true已连接，false未连接
 */
bool bsp_bt_is_connected(void);

/**
 * @brief 当前已连接设备的名称
 */
const char *bsp_bt_get_connected_name(void);

/**
 * @brief 当前已连接设备的地址 (last successful A2DP connect target).
 *
 * 即使现在没连接, 如果之前连接过会被记住, 用于在另一台设备"抢走"耳机后重连.
 * @return 字符串地址, 6 字节格式; 失败时返回 NULL.
 */
const char *bsp_bt_get_connected_addr(void);

/**
 * @brief 设置连接事件回调
 */
void bsp_bt_set_event_cb(bt_event_cb_t cb);

/**
 * @brief 启用/禁用"被抢走后自动重连"功能.
 *
 * 默认开启. 当 A2DP 连接断开且原因不是用户主动调用 bsp_bt_disconnect() 时,
 * 模块会尝试在 1s/3s/7s/15s/30s 这样的退避间隔后重新连接同一台已配对设备.
 * 用户主动断开不会触发重连.
 *
 * @param enable true=启用自动重连
 */
void bsp_bt_set_auto_reconnect(bool enable);

/**
 * @brief 启用/禁用"开启蓝牙后自动回连上次设备".
 *
 * 默认开启. bsp_bt_init() 成功后, 若 NVS 中记录过上次成功连接的设备,
 * 会通过工作线程延迟 ~1.5s 自动发起 A2DP 连接; 失败后进入与断线
 * 重连相同的 backoff 流程.
 */
void bsp_bt_set_auto_connect_last(bool enable);

/**
 * @brief 发送音频数据到蓝牙A2DP
 * @param data 音频数据
 * @param len  数据长度(字节)
 * @return ESP_OK 成功，ESP_ERR_NOT_SUPPORTED表示未启用蓝牙
 */
esp_err_t bsp_bt_a2dp_send_data(const uint8_t *data, size_t len);

/**
 * @brief 把外部 PCM 数据推入 A2DP ring buffer (供 audio_player 调用).
 *
 * 当蓝牙已连接时, audio_player 应把每帧解码后的 PCM 数据同时推入 ring buffer,
 * Bluedroid SBC encoder 会按自己的节奏从 ring buffer 拉取.
 * 未连接或 ring 满时返回 0 (调用方应继续把 PCM 写入 I2S, 不做其他动作).
 *
 * @param data PCM 数据 (16-bit stereo, 44.1k/48k)
 * @param len  字节数
 * @return 实际写入字节数 (0 表示 ring 满或蓝牙未初始化)
 */
size_t bsp_bt_pcm_ring_push(const uint8_t *data, size_t len);

/**
 * @brief 阻塞版 push: 内部循环等 ring 有空间, 最多等 max_wait_ms.
 *        适用于 audio_player 这种"宁可等也不丢数据"的实时生产者.
 *
 * @return 实际写入字节数; < len 表示到时间了还没塞下 (按时间窗放弃).
 */
size_t bsp_bt_pcm_ring_push_blocking(const uint8_t *data, size_t len,
                                     uint32_t max_wait_ms);

/**
 * @brief 查询 ring 当前已填充字节数 (用于上层做节流观测).
 */
size_t bsp_bt_pcm_ring_fill(void);

/**
 * @brief 查询 ring 总容量.
 */
size_t bsp_bt_pcm_ring_capacity(void);

/**
 * @brief 复位 A2DP PCM ring buffer (切歌/暂停时调用, 避免残留 PCM 混入下一首).
 */
void bsp_bt_pcm_ring_reset(void);

/**
 * @brief 预填充 ring buffer: 用静音 PCM (0) 填 ring.
 *
 * 在 BT 连接后、A2DP 媒体传输启动前调用.
 * 给 SBC encoder 足够的初始缓冲, 避免播放开始后立刻 underflow.
 *
 * @param bytes 要填入的静音字节数 (建议 32000 字节 = ~700ms @ 44.1k stereo).
 * @return 实际填入的字节数.
 */
size_t bsp_bt_pcm_ring_prefill(size_t bytes);

/**
 * @brief 获取 BT 音频音量 (0-100).
 *
 * BT 路径通过软件增益实现，I2S 路径通过 CS43131 硬件实现。
 */
uint8_t bsp_bt_get_volume(void);

/**
 * @brief 设置 BT 音频音量 (0-100).
 *
 * 同时同步到 CS43131 硬件寄存器（如果已初始化）。
 * BT 路径的音量通过在 bsp_bt_pcm_ring_push() 中应用固定点增益实现。
 */
void bsp_bt_set_volume(uint8_t volume);

/**
 * @brief 启动淡入: 在 A2DP 媒体传输开始后、真实音频开始推送时调用.
 *
 * 淡入持续约 20ms (882 帧 @ 44.1kHz)，线性从 0 增益到当前音量值.
 * 配合 64KB 预填充静音 + 1s MEDIA_START 延迟，消除静音 pop 杂音.
 */
void bsp_bt_start_fade_in(void);

/**
 * @brief 开始BLE扫描
 * @param cb  发现设备时的回调函数
 * @param duration_sec  扫描持续时间(秒), 0=默认5秒
 * @return ESP_OK 成功，ESP_ERR_NOT_SUPPORTED表示未启用蓝牙
 */
esp_err_t bsp_bt_scan_start(ble_scan_cb_t cb, uint32_t duration_sec);

/**
 * @brief 停止BLE扫描
 */
void bsp_bt_scan_stop(void);

/**
 * @brief 蓝牙是否正在扫描 (BLE 或 经典BT)
 */
bool bsp_bt_is_scanning(void);

/**
 * @brief 开始经典蓝牙设备发现 (用于搜索蓝牙耳机/扬声器)
 *
 * 经典蓝牙发现会广播查询, 大多数 A2DP 音频接收器 (耳机/音箱)
 * 都会响应并返回设备名称, RSSI, Class of Device。
 *
 * @param cb  发现设备时的回调函数
 * @param duration_sec  扫描持续时间(秒), 0=默认10秒, 最大 30 秒
 * @return ESP_OK 成功，ESP_ERR_NOT_SUPPORTED表示未启用蓝牙
 */
esp_err_t bsp_bt_classic_scan_start(bt_classic_scan_cb_t cb, uint32_t duration_sec);

/**
 * @brief 停止经典蓝牙发现
 */
void bsp_bt_classic_scan_stop(void);

/**
 * @brief 蓝牙是否正在扫描经典设备
 */
bool bsp_bt_classic_is_scanning(void);

/**
 * @brief 通过地址请求远端设备名称 (异步)
 * @note 名称将通过 bt_classic_scan_cb 回调返回
 */
esp_err_t bsp_bt_remote_dev_name_get(const char *addr);

/**
 * @brief 判断 Class of Device 是否为音频设备 (耳机/音箱)
 */
bool bsp_bt_cod_is_audio(uint32_t cod);

/**
 * @brief 获取本机蓝牙地址字符串
 */
const char *bsp_bt_get_local_addr(void);

/* =================== 已配对设备持久化 (NVS) =================== */

/**
 * @brief 加载已配对设备列表
 *
 * 从 NVS 中读取之前保存的所有已配对设备。
 *
 * @param out_arr 输出缓冲数组 (调用者分配)
 * @param max     数组最大容量
 * @return 实际写入的设备数
 */
int bsp_bt_load_paired_list(bt_paired_dev_t *out_arr, int max);

/**
 * @brief 添加/更新一个已配对设备
 *
 * 如果该地址已存在，则更新其名称。
 *
 * @return ESP_OK 成功
 */
esp_err_t bsp_bt_save_paired(const char *addr, const char *name);

/**
 * @brief 删除一个已配对设备
 *
 * 同时会调用 esp_bt_gap_remove_bond_device 移除底层绑定记录。
 *
 * @param addr 目标设备地址
 * @return ESP_OK 成功
 */
esp_err_t bsp_bt_remove_paired(const char *addr);

/**
 * @brief 获取已配对设备的数量
 */
int bsp_bt_get_paired_count(void);

/**
 * @brief 通过索引获取已配对设备
 *
 * @param index 索引 (0 ~ bsp_bt_get_paired_count()-1)
 * @param out   输出设备信息
 * @return true 成功, false 索引越界
 */
bool bsp_bt_get_paired_by_index(int index, bt_paired_dev_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BSP_BT_H */
