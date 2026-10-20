/**
 * @file ui_bluetooth.c
 * @brief 蓝牙连接页面 (适配 262x928 竖屏) - 类手机版
 *
 * 整体设计:
 *   - 顶部: 标题 + 蓝牙总开关 + 当前状态卡片
 *   - 已配对的设备列表 (从NVS加载, 点击触发 bsp_bt_connect)
 *   - 扫描到的蓝牙列表 (实时发现, 点击 → 确认弹窗 → 配对并连接)
 *   - 底部: 扫描按钮 (放在扫描区上方紧凑)
 *
 * 弹窗: "配对并连接" 对话框, 类手机Style:
 *   - 大号蓝牙图标 + 设备名 + MAC
 *   - 取消 / 配对并连接 双按钮
 */
#include "ui_bluetooth.h"
#include "ui_lvgl_async.h"
#include "bsp_bt.h"
#include "bsp_wireless_mgr.h"
#include "bsp_theme.h"
#include "ui_gesture.h"
#include "ui_navbar.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_bluetooth";

/* 屏幕尺寸 */
#define SCREEN_W       262
#define SCREEN_H       928
#define TITLE_BAR_H    40
#define NAVBAR_H_BT    52

/* 主题颜色 */
static inline lv_color_t theme_bg(void)     { return bsp_theme_colors()->bg; }
static inline lv_color_t theme_card(void)   { return bsp_theme_colors()->card; }
static inline lv_color_t theme_accent(void) { return bsp_theme_colors()->accent; }

#define COLOR_BG       theme_bg()
#define COLOR_CARD     theme_card()
#define COLOR_ACCENT   theme_accent()
#define COLOR_WHITE    lv_color_hex(0xFFFFFF)
#define COLOR_GRAY     lv_color_hex(0x888888)
#define COLOR_DARK_BG  lv_color_hex(0x101020)

/* 字体 */
extern lv_font_t esp_2;
// 蓝牙UI前置函数声明，解决顺序报错
static void _bt_event_async_cb(void *p);
static void refresh_paired_list(void);
static void _lv_paired_item_click(lv_event_t *e);
/* 设备数量上限 */
#define MAX_BLE_DEVICES 16

/* ======================== 页面状态 ======================== */
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *switch_obj;
    lv_obj_t *status_lbl;
    lv_obj_t *scan_btn;
    lv_obj_t *paired_list;   /* 已配对列表 */
    lv_obj_t *scan_list;     /* 扫描结果列表 */
    bool scanning;
} bt_page_t;

static bt_page_t s_bt_page = {0};

/* 蓝牙协议栈启动完成后自动发起的扫描 (扫描时栈未就绪的场景) */
static bool s_pending_scan = false;

/* 扫描结果共享缓冲 (BT线程写, LVGL读) */
typedef struct {
    ble_device_t devs[MAX_BLE_DEVICES];
    int count;
    /* 点击事件用: 排序后的地址序列 (与列表显示顺序一致) */
    char sorted_addr[MAX_BLE_DEVICES][18];
} ble_scan_buf_t;

static ble_scan_buf_t s_scan_buf = {0};
static SemaphoreHandle_t s_scan_mutex = NULL;

/* 已配对列表 (启动时从 NVS 加载, 便于回调查找) */
static bt_paired_dev_t s_paired_devs[MAX_BLE_DEVICES];
static int s_paired_count = 0;

/* ======================== 确认配对弹窗 ======================== */
typedef struct {
    lv_obj_t *screen;
    char addr[18];
    char name[64];
} bt_pair_dlg_t;

static bt_pair_dlg_t s_dlg = {0};

/* ======================== BT 事件 trampoline ======================== */
/* bsp_bt_set_event_cb 的回调运行在 BT 线程上下文, 不能直接操作LVGL,
 * 通过 lv_async_call 中转到 LVGL 线程. */
typedef struct {
    bool connected;
    char name[64];
} bt_event_msg_t;

/* 静态环形消息池: 固定 8 条, 避免每次事件都做小块 heap 分配.
 * 生产者 = BT 线程 (单线程), 消费者 = LVGL 线程 (单线程),
 * 槽位标志为单字节读写, 跨线程安全. 池满时丢弃事件 (连接状态变化极低频). */
#define BT_MSG_POOL_LEN 8

static bt_event_msg_t s_bt_msg_pool[BT_MSG_POOL_LEN];
static volatile bool s_bt_msg_used[BT_MSG_POOL_LEN];
static int s_bt_msg_next = 0;

static bt_event_msg_t *bt_msg_alloc(void)
{
    for (int i = 0; i < BT_MSG_POOL_LEN; i++) {
        int idx = (s_bt_msg_next + i) % BT_MSG_POOL_LEN;
        if (!s_bt_msg_used[idx]) {
            s_bt_msg_used[idx] = true;
            s_bt_msg_next = (idx + 1) % BT_MSG_POOL_LEN;
            return &s_bt_msg_pool[idx];
        }
    }
    return NULL;
}

static void bt_msg_release(bt_event_msg_t *msg)
{
    if (msg && msg >= s_bt_msg_pool && msg < s_bt_msg_pool + BT_MSG_POOL_LEN) {
        s_bt_msg_used[msg - s_bt_msg_pool] = false;
    }
}

static void dispatch_bt_event(bool connected, const char *device_name)
{
    bt_event_msg_t *msg = bt_msg_alloc();
    if (!msg) return;
    msg->connected = connected;
    if (device_name) {
        strncpy(msg->name, device_name, sizeof(msg->name) - 1);
        msg->name[sizeof(msg->name) - 1] = '\0';
    } else {
        msg->name[0] = '\0';
    }
    ui_lvgl_async_call(_bt_event_async_cb, msg);
}

static void _bt_event_async_cb(void *p)
{
    bt_event_msg_t *msg = (bt_event_msg_t *)p;
    if (!msg) return;

    if (s_bt_page.status_lbl) {
        if (msg->connected) {
            if (msg->name[0]) {
                lv_label_set_text_fmt(s_bt_page.status_lbl,
                                      "已连接: %s", msg->name);
            } else {
                lv_label_set_text(s_bt_page.status_lbl, "已连接");
            }
        } else {
            lv_label_set_text(s_bt_page.status_lbl, "未连接");
        }
    }
    refresh_paired_list();
    bt_msg_release(msg);
}

/* ======================== 导航回调 ======================== */
static void swipe_back_to_settings(void)
{
    extern lv_obj_t *ui_settings_get_screen(void);
    lv_obj_t *parent = ui_settings_get_screen();
    if (parent) {
        ui_navbar_replace_top(parent, false);
    }
}

static void swipe_back_from_dlg(void)
{
    if (s_bt_page.screen) lv_screen_load(s_bt_page.screen);
}

/* ======================== 已配对列表刷新 ======================== */

static void reload_paired_from_nvs(void)
{
    s_paired_count = bsp_bt_load_paired_list(s_paired_devs, MAX_BLE_DEVICES);
    ESP_LOGD(TAG, "已配对设备重载: %d", s_paired_count);
}

void refresh_paired_list(void)
{
    if (s_bt_page.paired_list == NULL) return;

    lv_obj_clean(s_bt_page.paired_list);

    reload_paired_from_nvs();

    if (s_paired_count == 0) {
        lv_obj_t *hint = lv_list_add_text(s_bt_page.paired_list,
                                          "暂无已配对设备\n请先在下方\"扫描到的蓝牙\"中点击配对");
        lv_obj_set_style_text_font(hint, &esp_2, 0);
        lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    for (int i = 0; i < s_paired_count; i++) {
        const char *name = s_paired_devs[i].name[0]
                          ? s_paired_devs[i].name
                          : s_paired_devs[i].addr;

        char buf[96];
        snprintf(buf, sizeof(buf), "%s\n%s",
                 name, s_paired_devs[i].addr);

        lv_obj_t *btn = lv_list_add_btn(s_bt_page.paired_list,
                                        LV_SYMBOL_BLUETOOTH, buf);
        lv_obj_set_style_text_font(btn, &esp_2, 0);

        /* 当前已连接的设备 ACCENT高亮 */
        if (bsp_bt_is_connected()) {
            const char *cur = bsp_bt_get_connected_name();
            /* 若有名称则按名称匹配, 否则按唯一已配对列表当作当前连接 */
            if (cur && cur[0]) {
                if (strcmp(cur, name) == 0) {
                    lv_obj_set_style_text_color(btn, COLOR_ACCENT, 0);
                    lv_obj_set_style_bg_color(btn, COLOR_ACCENT, 0);
                    lv_obj_set_style_bg_opa(btn, LV_OPA_30, 0);
                } else {
                    lv_obj_set_style_text_color(btn, COLOR_WHITE, 0);
                    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
                }
            } else {
                /* 连接设备名未知 - 高亮第一个让用户有视觉提示 */
                if (i == 0) {
                    lv_obj_set_style_text_color(btn, COLOR_ACCENT, 0);
                } else {
                    lv_obj_set_style_text_color(btn, COLOR_WHITE, 0);
                }
                lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
            }
        } else {
            lv_obj_set_style_text_color(btn, COLOR_WHITE, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        }

        /* user_data 直接存放已配对列表中数组元素的地址指针 */
        lv_obj_set_user_data(btn, (void *)&s_paired_devs[i]);
        lv_obj_add_event_cb(btn, _lv_paired_item_click,
                            LV_EVENT_CLICKED, NULL);
    }
}

/* 已配对项点击: 弹出"管理"对话框 (连接 / 取消配对).
 *
 * 这里不直接 bsp_bt_connect(), 因为:
 *   1. 用户可能想取消配对.
 *   2. 手机风格的多按钮弹窗体验更佳.
 */
static void _lv_paired_item_click(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    bt_paired_dev_t *dev = (bt_paired_dev_t *)lv_obj_get_user_data(btn);
    if (!dev || !dev->addr[0]) return;

    /* 缓存到模块静态, 弹窗按钮回调使用 */
    extern void ui_bluetooth_show_paired_action_dialog(bt_paired_dev_t *dev);
    ui_bluetooth_show_paired_action_dialog(dev);
}

/* ======================== 扫描列表 ======================== */

/* 前向声明 */
static void _lv_scan_item_click(lv_event_t *e);

static void refresh_scan_list(void)
{
    if (s_bt_page.scan_list == NULL) return;
    if (!s_scan_mutex) return;
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    lv_obj_clean(s_bt_page.scan_list);

    if (s_scan_buf.count == 0) {
        lv_obj_t *h = lv_list_add_text(s_bt_page.scan_list,
            "暂未发现设备\n请先点击\"扫描设备\"");
        lv_obj_set_style_text_font(h, &esp_2, 0);
        lv_obj_set_style_text_color(h, COLOR_GRAY, 0);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        xSemaphoreGive(s_scan_mutex);
        return;
    }

    /* 排序: 音频设备 + RSSI + 有名称 */
    int n = s_scan_buf.count;
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            int a_score = (s_scan_buf.devs[i].is_audio_device ? 1000 : 0) +
                          s_scan_buf.devs[i].rssi +
                          (s_scan_buf.devs[i].name[0] ? 200 : 0);
            int b_score = (s_scan_buf.devs[j].is_audio_device ? 1000 : 0) +
                          s_scan_buf.devs[j].rssi +
                          (s_scan_buf.devs[j].name[0] ? 200 : 0);
            if (b_score > a_score) {
                ble_device_t t = s_scan_buf.devs[i];
                s_scan_buf.devs[i] = s_scan_buf.devs[j];
                s_scan_buf.devs[j] = t;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        size_t cap = sizeof(s_scan_buf.sorted_addr[i]);
        size_t len = strnlen(s_scan_buf.devs[i].addr, cap - 1);
        memcpy(s_scan_buf.sorted_addr[i], s_scan_buf.devs[i].addr, len);
        s_scan_buf.sorted_addr[i][len] = '\0';
    }
    xSemaphoreGive(s_scan_mutex);

    for (int i = 0; i < n; i++) {
        const char *name = s_scan_buf.devs[i].name[0]
                          ? s_scan_buf.devs[i].name
                          : s_scan_buf.devs[i].addr;

        const char *type_tag = "";
        if (s_scan_buf.devs[i].is_audio_device)      type_tag = " [音频]";
        else if (s_scan_buf.devs[i].dev_type == 2)   type_tag = " [BLE]";
        else if (s_scan_buf.devs[i].dev_type == 1)   type_tag = " [经典]";

        char buf[96];
        snprintf(buf, sizeof(buf), "%s%s\n%d dBm",
                 name, type_tag, s_scan_buf.devs[i].rssi);

        const char *icon = s_scan_buf.devs[i].is_audio_device
                         ? LV_SYMBOL_AUDIO
                         : LV_SYMBOL_BLUETOOTH;

        lv_obj_t *btn = lv_list_add_btn(s_bt_page.scan_list, icon, buf);
        lv_obj_set_style_text_font(btn, &esp_2, 0);
        lv_obj_set_style_text_color(btn,
            s_scan_buf.devs[i].is_audio_device ? COLOR_ACCENT : COLOR_WHITE, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);

        /* user_data 存储地址字符串指针 (排序前的地址在 devs[], 与 sorted_addr[i] 等价) */
        lv_obj_set_user_data(btn,
                             (void *)s_scan_buf.sorted_addr[i]);
        lv_obj_add_event_cb(btn, _lv_scan_item_click, LV_EVENT_CLICKED, NULL);
    }
}

/* 扫描项点击: 跳转确认弹窗 */
static void _lv_scan_item_click(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    if (btn == NULL) return;

    const char *addr = (const char *)lv_obj_get_user_data(btn);
    if (!addr || !addr[0]) return;

    /* 从 s_scan_buf 中按地址查找名称 */
    const char *name = NULL;
    if (s_scan_mutex && xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < s_scan_buf.count; i++) {
            if (strcmp(s_scan_buf.devs[i].addr, addr) == 0) {
                if (s_scan_buf.devs[i].name[0]) {
                    strncpy(s_dlg.name, s_scan_buf.devs[i].name,
                            sizeof(s_dlg.name) - 1);
                    s_dlg.name[sizeof(s_dlg.name) - 1] = '\0';
                    name = s_dlg.name;
                }
                break;
            }
        }
        xSemaphoreGive(s_scan_mutex);
    }
    if (!name) {
        s_dlg.name[0] = '\0';
        name = addr;
    }
    strncpy(s_dlg.addr, addr, sizeof(s_dlg.addr) - 1);
    s_dlg.addr[sizeof(s_dlg.addr) - 1] = '\0';

    ESP_LOGI(TAG, "点击扫描设备: %s (%s)", addr, name);

    extern lv_obj_t *create_bt_pair_dialog(const char *name, const char *addr);
    lv_obj_t *scr = create_bt_pair_dialog(name, addr);
    lv_screen_load(scr);
}

static void refresh_scan_list_async(void *unused) { (void)unused; refresh_scan_list(); }

/* BLE 发现回调 (BT 线程) */
static void on_ble_device_found(const ble_device_t *dev)
{
    if (!dev) return;
    if (!s_scan_mutex) return;
    if (xSemaphoreTake(s_scan_mutex, 0) != pdTRUE) return;

    for (int i = 0; i < s_scan_buf.count; i++) {
        if (strcmp(s_scan_buf.devs[i].addr, dev->addr) == 0) {
            s_scan_buf.devs[i].rssi = dev->rssi;
            if (dev->name[0] && !s_scan_buf.devs[i].name[0]) {
                size_t cap = sizeof(s_scan_buf.devs[i].name);
                size_t len = strnlen(dev->name, cap - 1);
                memcpy(s_scan_buf.devs[i].name, dev->name, len);
                s_scan_buf.devs[i].name[len] = '\0';
            }
            if (dev->cod) {
                s_scan_buf.devs[i].cod = dev->cod;
                s_scan_buf.devs[i].is_audio_device = dev->is_audio_device;
            }
            if (dev->dev_type) s_scan_buf.devs[i].dev_type = dev->dev_type;
            xSemaphoreGive(s_scan_mutex);
            ui_lvgl_async_call(refresh_scan_list_async, NULL);
            return;
        }
    }
    if (s_scan_buf.count < MAX_BLE_DEVICES) {
        s_scan_buf.devs[s_scan_buf.count++] = *dev;
    }
    xSemaphoreGive(s_scan_mutex);
    ui_lvgl_async_call(refresh_scan_list_async, NULL);
}

static void ble_scan_done_cb(void *unused)
{
    (void)unused;
    s_bt_page.scanning = false;
    refresh_scan_list();
}

static TimerHandle_t s_scan_to_tmr = NULL;

static void ble_scan_timeout_cb(TimerHandle_t tmr)
{
    (void)tmr;
    if (!s_bt_page.scanning) return;
    ESP_LOGW(TAG, "BT扫描超时, 停止");
    bsp_bt_classic_scan_stop();
    bsp_bt_scan_stop();
    ui_lvgl_async_call(ble_scan_done_cb, NULL);
}

/* ======================== 控件回调 ======================== */
static void cb_scan_bt(lv_event_t *e)
{
    (void)e;
    if (s_bt_page.scanning) {
        ESP_LOGW(TAG, "已在扫描中");
        return;
    }

    if (!s_scan_mutex) s_scan_mutex = xSemaphoreCreateMutex();
    if (s_scan_mutex && xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_scan_buf.count = 0;
        memset(s_scan_buf.devs, 0, sizeof(s_scan_buf.devs));
        memset(s_scan_buf.sorted_addr, 0, sizeof(s_scan_buf.sorted_addr));
        xSemaphoreGive(s_scan_mutex);
    }

    s_bt_page.scanning = true;

    if (s_bt_page.scan_list) {
        lv_obj_clean(s_bt_page.scan_list);
        lv_obj_t *hint = lv_list_add_text(s_bt_page.scan_list,
            "正在扫描... (最多 10 秒)");
        lv_obj_set_style_text_font(hint, &esp_2, 0);
        lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
    }

    if (!bsp_bt_is_initialized()) {
        s_bt_page.scanning = false;
        /* 蓝牙协议栈未就绪: 按需异步启动 (worker 线程执行),
         * 启动完成事件 (WLESS_EVT_BT_STARTED) 会自动续接扫描 */
        s_pending_scan = true;
        if (!bsp_wireless_bt_active()) {
            bsp_wireless_bt_start();
        }
        if (s_bt_page.scan_list) {
            lv_obj_clean(s_bt_page.scan_list);
            lv_obj_t *h = lv_list_add_text(s_bt_page.scan_list, "正在启动蓝牙，请稍候...");
            lv_obj_set_style_text_font(h, &esp_2, 0);
            lv_obj_set_style_text_color(h, COLOR_ACCENT, 0);
        }
        return;
    }

    esp_err_t ret = bsp_bt_classic_scan_start(on_ble_device_found, 10);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "经典BT扫描失败, 尝试BLE: %s", esp_err_to_name(ret));
        ret = bsp_bt_scan_start(on_ble_device_found, 8);
    }

    if (ret != ESP_OK) {
        s_bt_page.scanning = false;
        if (s_bt_page.scan_list) {
            lv_obj_clean(s_bt_page.scan_list);
            const char *msg = "蓝牙未启用";
            if (ret == ESP_ERR_INVALID_STATE) msg = "请先打开蓝牙";
            lv_obj_t *h = lv_list_add_text(s_bt_page.scan_list, msg);
            lv_obj_set_style_text_font(h, &esp_2, 0);
            lv_obj_set_style_text_color(h, COLOR_ACCENT, 0);
        }
        return;
    }

    if (s_scan_to_tmr == NULL) {
        s_scan_to_tmr = xTimerCreate("ble_scan_to",
                                     pdMS_TO_TICKS(11000),
                                     pdFALSE, NULL, ble_scan_timeout_cb);
    } else {
        xTimerStop(s_scan_to_tmr, 0);
    }
    xTimerStart(s_scan_to_tmr, 0);
}

static void cb_bt_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
        /* 异步启动: worker 线程拉起 SDIO 链路 -> Bluedroid -> A2DP source,
         * 完成后经 ui_wireless_bridge 回调 ui_bluetooth_on_stack_event 更新 UI.
         * LVGL 线程不再被 bsp_bt_init() 阻塞数秒. */
        if (s_bt_page.status_lbl) {
            lv_label_set_text(s_bt_page.status_lbl, "正在启动蓝牙...");
        }
        esp_err_t ret = bsp_wireless_bt_start();
        if (ret != ESP_OK) {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
            if (s_bt_page.status_lbl) {
                lv_label_set_text(s_bt_page.status_lbl, "启动失败");
            }
        }
    } else {
        /* 异步停止: worker 会先断开 A2DP, 再完整反初始化 Bluedroid,
         * 回收协议栈内部 RAM 与 256KB PCM ring. */
        s_pending_scan = false;
        if (s_bt_page.status_lbl) {
            lv_label_set_text(s_bt_page.status_lbl, "正在关闭蓝牙...");
        }
        bsp_wireless_bt_stop();
    }
}

/* ======================== 无线管理器事件处理 ======================== */
/* 由 ui_wireless_bridge 在 LVGL 线程中调用, 根据协议栈启停结果刷新 UI. */
void ui_bluetooth_on_stack_event(wless_event_t evt, esp_err_t err)
{
    switch (evt) {
    case WLESS_EVT_BT_STARTED:
        if (s_bt_page.switch_obj) {
            lv_obj_add_state(s_bt_page.switch_obj, LV_STATE_CHECKED);
        }
        if (s_bt_page.status_lbl) {
            lv_label_set_text(s_bt_page.status_lbl, "蓝牙已开启");
        }
        refresh_paired_list();
        /* 扫描请求先于协议栈就绪: 自动续接 */
        if (s_pending_scan) {
            s_pending_scan = false;
            cb_scan_bt(NULL);
        }
        break;

    case WLESS_EVT_BT_START_FAIL:
        s_pending_scan = false;
        if (s_bt_page.switch_obj) {
            lv_obj_clear_state(s_bt_page.switch_obj, LV_STATE_CHECKED);
        }
        if (s_bt_page.status_lbl) {
            lv_label_set_text(s_bt_page.status_lbl,
                              (err == ESP_ERR_NO_MEM) ? "内存不足, 启动失败" : "蓝牙启动失败");
        }
        break;

    case WLESS_EVT_BT_STOPPED:
        if (s_bt_page.switch_obj) {
            lv_obj_clear_state(s_bt_page.switch_obj, LV_STATE_CHECKED);
        }
        if (s_bt_page.status_lbl) {
            lv_label_set_text(s_bt_page.status_lbl,
                              (err == ESP_OK) ? "蓝牙已关闭" : "蓝牙已停止");
        }
        break;

    default:
        break;
    }
}

/* ======================== 确认配对弹窗 ======================== */

static void dlg_cancel(lv_event_t *e) { (void)e; swipe_back_from_dlg(); }

static void dlg_confirm_pair(lv_event_t *e)
{
    (void)e;
    if (s_dlg.addr[0] == '\0') return;

    ESP_LOGI(TAG, "用户确认配对并连接: %s (%s)", s_dlg.name, s_dlg.addr);

    if (s_bt_page.status_lbl) {
        lv_label_set_text_fmt(s_bt_page.status_lbl,
                              "配对中... %s",
                              s_dlg.name[0] ? s_dlg.name : s_dlg.addr);
    }

    /* 真正发起配对 + 连接 (会在配对成功后NVS中添加一项) */
    esp_err_t ret = bsp_bt_pair_and_connect(s_dlg.addr,
                                            s_dlg.name[0] ? s_dlg.name : NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "发起配对失败: %s", esp_err_to_name(ret));
        if (s_bt_page.status_lbl) {
            lv_label_set_text(s_bt_page.status_lbl, "配对失败");
        }
    }

    /* 刷新已配对列表 (新增了一条) */
    refresh_paired_list();
    swipe_back_from_dlg();
}

/* 弹窗创建 (公开, 由 scan item click 调用) */
lv_obj_t *create_bt_pair_dialog(const char *name, const char *addr)
{
    if (s_dlg.screen) {
        lv_obj_del(s_dlg.screen);
        memset(&s_dlg, 0, sizeof(s_dlg));
    }

    s_dlg.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_dlg.screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_dlg.screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_dlg.screen, COLOR_WHITE, 0);

    ui_edge_swipe_add(s_dlg.screen, swipe_back_from_dlg);

    /* 标题 */
    lv_obj_t *title = lv_label_create(s_dlg.screen);
    lv_label_set_text(title, "配对并连接");
    lv_obj_set_style_text_font(title, &esp_2, 0);
    lv_obj_set_style_text_color(title, COLOR_WHITE, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *back = lv_label_create(s_dlg.screen);
    lv_label_set_text(back, "<");
    lv_obj_set_style_text_font(back, &esp_2, 0);
    lv_obj_set_style_text_color(back, COLOR_GRAY, 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 10, 14);

    /* 设备大圆形图标 */
    lv_obj_t *icon_box = lv_obj_create(s_dlg.screen);
    lv_obj_set_size(icon_box, 80, 80);
    lv_obj_align(icon_box, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_radius(icon_box, 40, 0);
    lv_obj_set_style_bg_color(icon_box, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon_box, 2, 0);
    lv_obj_set_style_border_color(icon_box, COLOR_ACCENT, 0);
    lv_obj_set_style_pad_all(icon_box, 0, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(icon, &esp_2, 0);
    lv_obj_set_style_text_color(icon, COLOR_WHITE, 0);
    lv_obj_center(icon);

    /* 设备名 */
    lv_obj_t *name_lbl = lv_label_create(s_dlg.screen);
    lv_obj_set_style_text_font(name_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(name_lbl, COLOR_WHITE, 0);
    lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(name_lbl, SCREEN_W - 20);
    lv_label_set_text(name_lbl, name ? name : "(未知设备)");
    lv_obj_align(name_lbl, LV_ALIGN_TOP_MID, 0, 156);

    /* MAC 地址 */
    lv_obj_t *addr_lbl = lv_label_create(s_dlg.screen);
    lv_obj_set_style_text_font(addr_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(addr_lbl, COLOR_GRAY, 0);
    lv_obj_set_style_text_align(addr_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(addr_lbl, SCREEN_W - 20);
    lv_label_set_text(addr_lbl, addr ? addr : "");
    lv_obj_align(addr_lbl, LV_ALIGN_TOP_MID, 0, 184);

    /* 描述 */
    lv_obj_t *desc = lv_label_create(s_dlg.screen);
    lv_label_set_text(desc,
        "是否与此设备进行配对?\n配对成功后会在\"已配对设备\"中保留记录,\n下次可直接点击连接。");
    lv_obj_set_style_text_font(desc, &esp_2, 0);
    lv_obj_set_style_text_color(desc, COLOR_GRAY, 0);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(desc, SCREEN_W - 20);
    lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, 220);

    /* 取消按钮 */
    lv_obj_t *cancel_btn = lv_btn_create(s_dlg.screen);
    lv_obj_set_size(cancel_btn, (SCREEN_W - 30) / 2, 40);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 10, 320);
    lv_obj_set_style_bg_color(cancel_btn, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_add_event_cb(cancel_btn, dlg_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "取消");
    lv_obj_set_style_text_font(cancel_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(cancel_lbl, COLOR_WHITE, 0);
    lv_obj_center(cancel_lbl);

    /* 确认按钮 */
    lv_obj_t *ok_btn = lv_btn_create(s_dlg.screen);
    lv_obj_set_size(ok_btn, (SCREEN_W - 30) / 2, 40);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -10, 320);
    lv_obj_set_style_bg_color(ok_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(ok_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ok_btn, 6, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_add_event_cb(ok_btn, dlg_confirm_pair, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, "配对并连接");
    lv_obj_set_style_text_font(ok_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(ok_lbl, COLOR_WHITE, 0);
    lv_obj_center(ok_lbl);

    if (name) {
        strncpy(s_dlg.name, name, sizeof(s_dlg.name) - 1);
        s_dlg.name[sizeof(s_dlg.name) - 1] = '\0';
    }
    if (addr) {
        strncpy(s_dlg.addr, addr, sizeof(s_dlg.addr) - 1);
        s_dlg.addr[sizeof(s_dlg.addr) - 1] = '\0';
    }

    return s_dlg.screen;
}

/* ======================== 已配对设备 操作弹窗 (连接 / 取消配对) ======================== */

static void dlg_paired_cancel(lv_event_t *e) { (void)e; swipe_back_from_dlg(); }

static void dlg_paired_connect(lv_event_t *e)
{
    (void)e;
    /* 从 s_dlg (复用 pair dialog 的字段) 拿 addr */
    if (s_dlg.addr[0] == '\0') return;

    if (s_bt_page.status_lbl) {
        const char *nm = s_dlg.name[0] ? s_dlg.name : s_dlg.addr;
        lv_label_set_text_fmt(s_bt_page.status_lbl,
                              "连接中... %s", nm);
    }

    esp_err_t ret = bsp_bt_connect(s_dlg.addr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "连接请求失败: %s", esp_err_to_name(ret));
    }
    swipe_back_from_dlg();
}

static void dlg_paired_unpair(lv_event_t *e)
{
    (void)e;
    if (s_dlg.addr[0] == '\0') return;

    ESP_LOGI(TAG, "用户取消配对: %s", s_dlg.addr);

    /* 1) 如果当前连的是它, 先断开 (避免 Bluedroid 仍保留 bond). */
    if (bsp_bt_is_connected()) {
        const char *cur = bsp_bt_get_connected_addr();
        if (cur && strcmp(cur, s_dlg.addr) == 0) {
            bsp_bt_disconnect();
        } else if (cur == NULL) {
            /* 当前连接设备名未知, 但地址未知不一定是同一设备, 保险起见不主动断 */
        }
    }

    /* 2) 从 NVS 移除, 同时也调用底层 remove_bond_device() */
    esp_err_t ret = bsp_bt_remove_paired(s_dlg.addr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "取消配对失败: %s", esp_err_to_name(ret));
    }

    /* 3) 状态提示 */
    if (s_bt_page.status_lbl) {
        lv_label_set_text_fmt(s_bt_page.status_lbl,
                              "已取消配对: %s",
                              s_dlg.name[0] ? s_dlg.name : s_dlg.addr);
    }

    /* 4) 刷新已配对列表 + 返回 */
    refresh_paired_list();
    swipe_back_from_dlg();
}

void ui_bluetooth_show_paired_action_dialog(bt_paired_dev_t *dev)
{
    if (!dev || !dev->addr[0]) return;

    /* 复用 s_dlg 状态, 先清空旧内容 */
    if (s_dlg.screen) {
        lv_obj_del(s_dlg.screen);
        memset(&s_dlg, 0, sizeof(s_dlg));
    }

    s_dlg.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_dlg.screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_dlg.screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_dlg.screen, COLOR_WHITE, 0);
    ui_edge_swipe_add(s_dlg.screen, swipe_back_from_dlg);

    /* 标题 */
    lv_obj_t *title = lv_label_create(s_dlg.screen);
    lv_label_set_text(title, "已配对设备");
    lv_obj_set_style_text_font(title, &esp_2, 0);
    lv_obj_set_style_text_color(title, COLOR_WHITE, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *back = lv_label_create(s_dlg.screen);
    lv_label_set_text(back, "<");
    lv_obj_set_style_text_font(back, &esp_2, 0);
    lv_obj_set_style_text_color(back, COLOR_GRAY, 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 10, 14);

    /* 设备圆形图标 */
    lv_obj_t *icon_box = lv_obj_create(s_dlg.screen);
    lv_obj_set_size(icon_box, 80, 80);
    lv_obj_align(icon_box, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_radius(icon_box, 40, 0);
    lv_obj_set_style_bg_color(icon_box, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon_box, 2, 0);
    lv_obj_set_style_border_color(icon_box, COLOR_ACCENT, 0);
    lv_obj_set_style_pad_all(icon_box, 0, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(icon, &esp_2, 0);
    lv_obj_set_style_text_color(icon, COLOR_WHITE, 0);
    lv_obj_center(icon);

    /* 名称 + MAC */
    lv_obj_t *name_lbl = lv_label_create(s_dlg.screen);
    lv_obj_set_style_text_font(name_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(name_lbl, COLOR_WHITE, 0);
    lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(name_lbl, SCREEN_W - 20);
    lv_label_set_text(name_lbl, dev->name[0] ? dev->name : "(未知)");
    lv_obj_align(name_lbl, LV_ALIGN_TOP_MID, 0, 152);

    lv_obj_t *addr_lbl = lv_label_create(s_dlg.screen);
    lv_obj_set_style_text_font(addr_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(addr_lbl, COLOR_GRAY, 0);
    lv_obj_set_style_text_align(addr_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(addr_lbl, SCREEN_W - 20);
    lv_label_set_text(addr_lbl, dev->addr);
    lv_obj_align(addr_lbl, LV_ALIGN_TOP_MID, 0, 180);

    /* 当前是否连着这条 */
    const char *conn_addr = bsp_bt_get_connected_addr();
    bool is_current = (conn_addr && strcmp(conn_addr, dev->addr) == 0);
    if (is_current) {
        lv_obj_t *cur = lv_label_create(s_dlg.screen);
        lv_label_set_text(cur, "当前已连接");
        lv_obj_set_style_text_font(cur, &esp_2, 0);
        lv_obj_set_style_text_color(cur, COLOR_ACCENT, 0);
        lv_obj_set_style_text_align(cur, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(cur, SCREEN_W - 20);
        lv_obj_align(cur, LV_ALIGN_TOP_MID, 0, 206);
    }

    /* "连接" 按钮 (左, 主色) */
    lv_obj_t *conn_btn = lv_btn_create(s_dlg.screen);
    lv_obj_set_size(conn_btn, (SCREEN_W - 30) / 2, 44);
    lv_obj_align(conn_btn, LV_ALIGN_TOP_LEFT, 10, 280);
    lv_obj_set_style_bg_color(conn_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(conn_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(conn_btn, 6, 0);
    lv_obj_set_style_shadow_width(conn_btn, 0, 0);
    lv_obj_set_style_border_width(conn_btn, 0, 0);
    lv_obj_add_event_cb(conn_btn, dlg_paired_connect, LV_EVENT_CLICKED, NULL);
    lv_obj_t *conn_lbl = lv_label_create(conn_btn);
    lv_label_set_text(conn_lbl, "连接");
    lv_obj_set_style_text_font(conn_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(conn_lbl, COLOR_WHITE, 0);
    lv_obj_center(conn_lbl);

    /* "取消配对" 按钮 (右, 危险色用红色边框提示) */
    lv_obj_t *unp_btn = lv_btn_create(s_dlg.screen);
    lv_obj_set_size(unp_btn, (SCREEN_W - 30) / 2, 44);
    lv_obj_align(unp_btn, LV_ALIGN_TOP_RIGHT, -10, 280);
    lv_obj_set_style_bg_color(unp_btn, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(unp_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(unp_btn, 6, 0);
    lv_obj_set_style_shadow_width(unp_btn, 0, 0);
    lv_obj_set_style_border_width(unp_btn, 1, 0);
    /* 用 ACCENT 作为边框颜色 (代表"警告") */
    lv_obj_set_style_border_color(unp_btn, COLOR_ACCENT, 0);
    lv_obj_add_event_cb(unp_btn, dlg_paired_unpair, LV_EVENT_CLICKED, NULL);
    lv_obj_t *unp_lbl = lv_label_create(unp_btn);
    lv_label_set_text(unp_lbl, "取消配对");
    lv_obj_set_style_text_font(unp_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(unp_lbl, COLOR_ACCENT, 0);
    lv_obj_center(unp_lbl);

    /* 底部"关闭"按钮 */
    lv_obj_t *close_btn = lv_btn_create(s_dlg.screen);
    lv_obj_set_size(close_btn, SCREEN_W - 20, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_MID, 0, 340);
    lv_obj_set_style_bg_color(close_btn, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_40, 0);
    lv_obj_set_style_radius(close_btn, 6, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, dlg_paired_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "关闭");
    lv_obj_set_style_text_font(close_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(close_lbl, COLOR_GRAY, 0);
    lv_obj_center(close_lbl);

    /* 缓存设备信息 (供按钮回调读取) */
    strncpy(s_dlg.addr, dev->addr, sizeof(s_dlg.addr) - 1);
    s_dlg.addr[sizeof(s_dlg.addr) - 1] = '\0';
    strncpy(s_dlg.name, dev->name, sizeof(s_dlg.name) - 1);
    s_dlg.name[sizeof(s_dlg.name) - 1] = '\0';

    lv_screen_load(s_dlg.screen);
}

/* ======================== 页面构建 ======================== */
void refresh_bt_page(void)
{
    if (s_bt_page.screen == NULL) return;
    bool active = bsp_wireless_bt_active() || bsp_bt_is_connected();
    if (s_bt_page.switch_obj) {
        if (active) {
            lv_obj_add_state(s_bt_page.switch_obj, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_bt_page.switch_obj, LV_STATE_CHECKED);
        }
    }
    if (s_bt_page.status_lbl) {
        if (bsp_bt_is_connected()) {
            const char *cur = bsp_bt_get_connected_name();
            lv_label_set_text_fmt(s_bt_page.status_lbl,
                                  "已连接: %s", cur ? cur : "蓝牙设备");
        } else if (active) {
            lv_label_set_text(s_bt_page.status_lbl, "蓝牙已开启");
        } else {
            lv_label_set_text(s_bt_page.status_lbl, "蓝牙未开启");
        }
    }
    refresh_paired_list();
}

lv_obj_t *ui_bluetooth_create_page(void)
{
    if (s_bt_page.screen) {
        refresh_bt_page();
        return s_bt_page.screen;
    }

    s_bt_page.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_bt_page.screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_bt_page.screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_bt_page.screen, COLOR_WHITE, 0);

    ui_edge_swipe_add(s_bt_page.screen, swipe_back_to_settings);

    /* 标题 */
    lv_obj_t *title = lv_label_create(s_bt_page.screen);
    lv_label_set_text(title, "蓝牙");
    lv_obj_set_style_text_font(title, &esp_2, 0);
    lv_obj_set_style_text_color(title, COLOR_WHITE, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *hint_swipe = lv_label_create(s_bt_page.screen);
    lv_label_set_text(hint_swipe, "<");
    lv_obj_set_style_text_font(hint_swipe, &esp_2, 0);
    lv_obj_set_style_text_color(hint_swipe, COLOR_GRAY, 0);
    lv_obj_align(hint_swipe, LV_ALIGN_TOP_LEFT, 8, 12);

    /* 蓝牙开关 */
    s_bt_page.switch_obj = lv_switch_create(s_bt_page.screen);
    lv_obj_set_size(s_bt_page.switch_obj, 40, 22);
    lv_obj_align(s_bt_page.switch_obj, LV_ALIGN_TOP_RIGHT, -10, 14);
    lv_obj_set_style_bg_color(s_bt_page.switch_obj, COLOR_GRAY, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bt_page.switch_obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bt_page.switch_obj, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bt_page.switch_obj, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_bt_page.switch_obj, cb_bt_switch, LV_EVENT_VALUE_CHANGED, NULL);

    /* 状态卡片 */
    lv_obj_t *status_box = lv_obj_create(s_bt_page.screen);
    lv_obj_set_size(status_box, SCREEN_W - 12, 48);
    lv_obj_set_pos(status_box, 6, 44);
    lv_obj_set_style_bg_color(status_box, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(status_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(status_box, 6, 0);
    lv_obj_set_style_border_width(status_box, 0, 0);
    lv_obj_set_style_pad_all(status_box, 8, 0);

    s_bt_page.status_lbl = lv_label_create(status_box);
    lv_label_set_text(s_bt_page.status_lbl, "未连接");
    lv_obj_set_style_text_font(s_bt_page.status_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(s_bt_page.status_lbl, COLOR_WHITE, 0);
    lv_obj_align(s_bt_page.status_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *bt_icon = lv_label_create(status_box);
    lv_label_set_text(bt_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(bt_icon, COLOR_ACCENT, 0);
    lv_obj_align(bt_icon, LV_ALIGN_RIGHT_MID, 0, 0);

    /* ===== "已配对的设备" 区 ===== */
    lv_obj_t *paired_title = lv_label_create(s_bt_page.screen);
    lv_label_set_text(paired_title, "已配对的设备");
    lv_obj_set_style_text_font(paired_title, &esp_2, 0);
    lv_obj_set_style_text_color(paired_title, COLOR_GRAY, 0);
    lv_obj_set_pos(paired_title, 8, 102);

    s_bt_page.paired_list = lv_list_create(s_bt_page.screen);
    lv_obj_set_size(s_bt_page.paired_list, SCREEN_W - 12, 174);
    lv_obj_set_pos(s_bt_page.paired_list, 6, 122);
    lv_obj_set_style_bg_color(s_bt_page.paired_list, COLOR_DARK_BG, 0);
    lv_obj_set_style_bg_opa(s_bt_page.paired_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bt_page.paired_list, 0, 0);
    lv_obj_set_style_radius(s_bt_page.paired_list, 6, 0);
    lv_obj_set_style_pad_all(s_bt_page.paired_list, 4, 0);
    lv_obj_set_style_text_font(s_bt_page.paired_list, &esp_2, 0);

    /* ===== "扫描到的蓝牙" 区 ===== */
    lv_obj_t *scan_title = lv_label_create(s_bt_page.screen);
    lv_label_set_text(scan_title, "扫描到的蓝牙");
    lv_obj_set_style_text_font(scan_title, &esp_2, 0);
    lv_obj_set_style_text_color(scan_title, COLOR_GRAY, 0);
    lv_obj_set_pos(scan_title, 8, 304);

    s_bt_page.scan_btn = lv_btn_create(s_bt_page.screen);
    lv_obj_set_size(s_bt_page.scan_btn, SCREEN_W - 12, 32);
    lv_obj_set_pos(s_bt_page.scan_btn, 6, 324);
    lv_obj_set_style_bg_color(s_bt_page.scan_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_radius(s_bt_page.scan_btn, 6, 0);
    lv_obj_set_style_shadow_width(s_bt_page.scan_btn, 0, 0);
    lv_obj_add_event_cb(s_bt_page.scan_btn, cb_scan_bt, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_lbl = lv_label_create(s_bt_page.scan_btn);
    lv_label_set_text(scan_lbl, LV_SYMBOL_REFRESH " 扫描设备");
    lv_obj_set_style_text_font(scan_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(scan_lbl, COLOR_WHITE, 0);
    lv_obj_center(scan_lbl);

    s_bt_page.scan_list = lv_list_create(s_bt_page.screen);
    lv_obj_set_size(s_bt_page.scan_list, SCREEN_W - 12,
                    SCREEN_H - 366 - TITLE_BAR_H - NAVBAR_H_BT);
    lv_obj_set_pos(s_bt_page.scan_list, 6, 366);
    lv_obj_set_style_bg_color(s_bt_page.scan_list, COLOR_DARK_BG, 0);
    lv_obj_set_style_bg_opa(s_bt_page.scan_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bt_page.scan_list, 0, 0);
    lv_obj_set_style_radius(s_bt_page.scan_list, 6, 0);
    lv_obj_set_style_pad_all(s_bt_page.scan_list, 4, 0);
    lv_obj_set_style_text_font(s_bt_page.scan_list, &esp_2, 0);

    lv_obj_t *hint = lv_list_add_text(s_bt_page.scan_list, "点击\"扫描设备\"开始搜索");
    lv_obj_set_style_text_font(hint, &esp_2, 0);
    lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);

    /* 注册 BT 事件回调 (通过 trampoline 安全切换线程) */
    bsp_bt_set_event_cb(dispatch_bt_event);

    refresh_bt_page();
    return s_bt_page.screen;
}

lv_obj_t *ui_bluetooth_get_screen(void)
{
    return s_bt_page.screen;
}

void ui_bluetooth_invalidate(void)
{
    if (s_bt_page.screen) {
        lv_obj_del(s_bt_page.screen);
        s_bt_page.screen = NULL;
        memset(&s_bt_page, 0, sizeof(s_bt_page));
    }
    if (s_dlg.screen) {
        lv_obj_del(s_dlg.screen);
        s_dlg.screen = NULL;
        memset(&s_dlg, 0, sizeof(s_dlg));
    }
    if (s_scan_mutex && xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_scan_buf.count = 0;
        memset(s_scan_buf.devs, 0, sizeof(s_scan_buf.devs));
        xSemaphoreGive(s_scan_mutex);
    }
}
