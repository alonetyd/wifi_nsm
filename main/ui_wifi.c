/**
 * @file ui_wifi.c
 * @brief WiFi连接页面 (适配 262x928 竖屏)
 *
 * 功能:
 * - 状态显示 (WiFi开关、IP)
 * - 扫描结果列表 (异步刷新)
 * - 点击SSID → 密码输入弹窗
 */
#include "ui_wifi.h"
#include "ui_lvgl_async.h"
#include "bsp_wifi.h"
#include "bsp_wireless_mgr.h"
#include "bsp_theme.h"
#include "ui_gesture.h"
#include "ui_navbar.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_wifi";

/* 屏幕尺寸 (与 ui_settings.c 一致) */
#define SCREEN_W       262
#define SCREEN_H       928
#define TITLE_BAR_H    40
#define NAVBAR_H_WIFI  52                    /* 底部导航栏高度 */
#define CONTENT_PAD    6
#define LIST_ITEM_H    44

/* 主题颜色 - 运行时获取 */
static inline lv_color_t theme_bg(void)     { return bsp_theme_colors()->bg; }
static inline lv_color_t theme_card(void)   { return bsp_theme_colors()->card; }
static inline lv_color_t theme_accent(void) { return bsp_theme_colors()->accent; }

#define COLOR_BG       theme_bg()
#define COLOR_CARD     theme_card()
#define COLOR_ACCENT   theme_accent()
#define COLOR_WHITE    lv_color_hex(0xFFFFFF)
#define COLOR_GRAY     lv_color_hex(0x888888)
#define COLOR_DARK_BG  lv_color_hex(0x202030)

/* 字体 */
extern lv_font_t esp_2;

/* WiFi页面全局状态 */
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *switch_obj;
    lv_obj_t *status_lbl;
    lv_obj_t *ip_lbl;
    lv_obj_t *scan_btn;
    lv_obj_t *scan_list;
    lv_obj_t *hint_lbl;
    bool scanning;
} wifi_page_t;

static wifi_page_t s_wifi_page = {0};

/* WiFi协议栈启动完成后自动发起的扫描 (扫描时栈未就绪的场景) */
static bool s_pending_scan = false;

/* 密码输入对话框状态 */
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *input_ta;
    lv_obj_t *ssid_lbl;
    char ssid[WIFI_SSID_MAX + 1];
} wifi_pwd_dlg_t;

static wifi_pwd_dlg_t s_pwd_dlg = {0};

/* 侧滑返回到设置主页 (通过导航栏替换栈顶, 避免栈增长) */
static void swipe_back_to_settings(void)
{
    extern lv_obj_t *ui_settings_get_screen(void);
    lv_obj_t *parent = ui_settings_get_screen();
    if (parent) {
        ui_navbar_replace_top(parent, false);
    }
}

/* ================================================================
 * 密码输入对话框
 * ================================================================ */

/* 侧滑手势回调: 无参包装 (ui_edge_swipe_add 要求 void(void)) */
static void cb_pwd_back_swipe(void)
{
    if (s_pwd_dlg.screen && s_wifi_page.screen) {
        lv_screen_load(s_wifi_page.screen);
    }
}

static void cb_pwd_back(lv_event_t *e)
{
    (void)e;
    cb_pwd_back_swipe();
}

static void cb_pwd_connect(lv_event_t *e)
{
    (void)e;
    if (s_pwd_dlg.ssid[0] == '\0') return;

    const char *pwd = lv_textarea_get_text(s_pwd_dlg.input_ta);
    if (!pwd) pwd = "";

    bsp_wifi_save_ap(s_pwd_dlg.ssid, pwd);
    bsp_wifi_connect(s_pwd_dlg.ssid, pwd);
    ESP_LOGI(TAG, "连接WiFi: %s", s_pwd_dlg.ssid);

    if (s_wifi_page.screen) lv_screen_load(s_wifi_page.screen);
}

static void cb_pwd_toggle(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    bool mode = !lv_textarea_get_password_mode(s_pwd_dlg.input_ta);
    lv_textarea_set_password_mode(s_pwd_dlg.input_ta, mode);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label) {
        lv_label_set_text(label, mode ? "显" : "隐");
    }
}

static void cb_pwd_screen_loaded(lv_event_t *e)
{
    (void)e;
    /* 密码页加载后自动弹出键盘 */
    if (s_pwd_dlg.input_ta) {
        lv_obj_t *kb = lv_keyboard_create(s_pwd_dlg.screen);
        lv_obj_set_size(kb, SCREEN_W, 280);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_keyboard_set_textarea(kb, s_pwd_dlg.input_ta);
    }
}

static void create_password_dialog(const char *ssid)
{
    s_pwd_dlg.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_pwd_dlg.screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_pwd_dlg.screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_pwd_dlg.screen, COLOR_WHITE, 0);

    /* 侧滑返回 */
    ui_edge_swipe_add(s_pwd_dlg.screen, cb_pwd_back_swipe);

    /* 标题 */
    lv_obj_t *title = lv_label_create(s_pwd_dlg.screen);
    lv_label_set_text(title, "WiFi 密码");
    lv_obj_set_style_text_font(title, &esp_2, 0);
    lv_obj_set_style_text_color(title, COLOR_WHITE, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* SSID显示 */
    s_pwd_dlg.ssid_lbl = lv_label_create(s_pwd_dlg.screen);
    char buf[WIFI_SSID_MAX + 32];
    snprintf(buf, sizeof(buf), "%s", ssid);
    lv_label_set_text(s_pwd_dlg.ssid_lbl, buf);
    lv_obj_set_style_text_font(s_pwd_dlg.ssid_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(s_pwd_dlg.ssid_lbl, COLOR_GRAY, 0);
    lv_obj_set_style_text_align(s_pwd_dlg.ssid_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_pwd_dlg.ssid_lbl, SCREEN_W - 20);
    lv_obj_align(s_pwd_dlg.ssid_lbl, LV_ALIGN_TOP_MID, 0, 38);

    /* 密码输入框 */
    s_pwd_dlg.input_ta = lv_textarea_create(s_pwd_dlg.screen);
    lv_obj_set_size(s_pwd_dlg.input_ta, SCREEN_W - 20, 40);
    lv_obj_align(s_pwd_dlg.input_ta, LV_ALIGN_TOP_MID, 0, 68);
    lv_textarea_set_placeholder_text(s_pwd_dlg.input_ta, "请输入密码");
    lv_textarea_set_password_mode(s_pwd_dlg.input_ta, true);
    lv_textarea_set_max_length(s_pwd_dlg.input_ta, WIFI_PASS_MAX);
    lv_obj_set_style_bg_color(s_pwd_dlg.input_ta, COLOR_CARD, 0);
    lv_obj_set_style_text_color(s_pwd_dlg.input_ta, COLOR_WHITE, 0);
    lv_obj_set_style_border_color(s_pwd_dlg.input_ta, COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(s_pwd_dlg.input_ta, 2, 0);
    lv_obj_set_style_radius(s_pwd_dlg.input_ta, 6, 0);
    lv_obj_set_style_pad_left(s_pwd_dlg.input_ta, 30, 0);

    /* 显隐切换 */
    lv_obj_t *toggle_btn = lv_btn_create(s_pwd_dlg.screen);
    lv_obj_set_size(toggle_btn, 36, 28);
    lv_obj_align(toggle_btn, LV_ALIGN_TOP_RIGHT, -10, 74);
    lv_obj_set_style_bg_color(toggle_btn, COLOR_CARD, 0);
    lv_obj_set_style_radius(toggle_btn, 4, 0);
    lv_obj_add_event_cb(toggle_btn, cb_pwd_toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_t *toggle_lbl = lv_label_create(toggle_btn);
    lv_label_set_text(toggle_lbl, "显");
    lv_obj_set_style_text_font(toggle_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(toggle_lbl, COLOR_WHITE, 0);
    lv_obj_center(toggle_lbl);

    /* 按钮行 */
    lv_obj_t *cancel_btn = lv_btn_create(s_pwd_dlg.screen);
    lv_obj_set_size(cancel_btn, (SCREEN_W - 30) / 2, 36);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 10, 118);
    lv_obj_set_style_bg_color(cancel_btn, COLOR_CARD, 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_add_event_cb(cancel_btn, cb_pwd_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "取消");
    lv_obj_set_style_text_color(cancel_lbl, COLOR_WHITE, 0);
    lv_obj_center(cancel_lbl);

    lv_obj_t *connect_btn = lv_btn_create(s_pwd_dlg.screen);
    lv_obj_set_size(connect_btn, (SCREEN_W - 30) / 2, 36);
    lv_obj_align(connect_btn, LV_ALIGN_TOP_RIGHT, -10, 118);
    lv_obj_set_style_bg_color(connect_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_radius(connect_btn, 6, 0);
    lv_obj_add_event_cb(connect_btn, cb_pwd_connect, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_lbl = lv_label_create(connect_btn);
    lv_label_set_text(connect_lbl, "连接");
    lv_obj_set_style_text_color(connect_lbl, COLOR_WHITE, 0);
    lv_obj_center(connect_lbl);

    /* 提示 */
    lv_obj_t *hint = lv_label_create(s_pwd_dlg.screen);
    lv_label_set_text(hint, "无密码可留空直接连接");
    lv_obj_set_style_text_font(hint, &esp_2, 0);
    lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 162);

    /* 保存SSID */
    strncpy(s_pwd_dlg.ssid, ssid, WIFI_SSID_MAX);
    s_pwd_dlg.ssid[WIFI_SSID_MAX] = '\0';

    /* 屏幕加载后弹出键盘 */
    lv_obj_add_event_cb(s_pwd_dlg.screen, cb_pwd_screen_loaded, LV_EVENT_SCREEN_LOADED, NULL);
}

/* ================================================================
 * 扫描列表渲染
 * ================================================================ */

static void cb_wifi_item_click(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *ssid = (const char *)lv_obj_get_user_data(btn);
    if (ssid && ssid[0]) {
        create_password_dialog(ssid);
        lv_screen_load(s_pwd_dlg.screen);
    }
}

/* 异步刷新: 由 bsp_wifi 在扫描完成时通过 LVGL async 回调触发 */
void ui_wifi_on_scan_done(void)
{
    if (s_wifi_page.scan_list == NULL) return;
    s_wifi_page.scanning = false;

    lv_obj_clean(s_wifi_page.scan_list);

    wifi_ap_info_t aps[WIFI_SCAN_MAX];
    int count = bsp_wifi_get_scan_results(aps, WIFI_SCAN_MAX);

    if (count <= 0) {
        lv_obj_t *hint = lv_list_add_text(s_wifi_page.scan_list, "未发现WiFi");
        lv_obj_set_style_text_font(hint, &esp_2, 0);
        lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);
        return;
    }

    /* 按 RSSI 降序排序 */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (aps[j].rssi > aps[i].rssi) {
                wifi_ap_info_t t = aps[i];
                aps[i] = aps[j];
                aps[j] = t;
            }
        }
    }

    for (int i = 0; i < count; i++) {
        char buf[WIFI_SSID_MAX + 32];
        const char *lock = (aps[i].authmode != 0) ? LV_SYMBOL_CLOSE : " ";
        snprintf(buf, sizeof(buf), "%s %s\n%d dBm",
                 lock, aps[i].ssid, aps[i].rssi);

        lv_obj_t *btn = lv_list_add_btn(s_wifi_page.scan_list, LV_SYMBOL_WIFI, buf);
        lv_obj_set_user_data(btn, (void *)aps[i].ssid);
        lv_obj_set_style_text_font(btn, &esp_2, 0);
        lv_obj_set_style_text_color(btn, COLOR_WHITE, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(btn, cb_wifi_item_click, LV_EVENT_CLICKED, NULL);
    }
}

/* 扫描超时回调 - 即使没收到 SDIO 事件也提示用户 */
static void wifi_scan_timeout_cb(TimerHandle_t tmr)
{
    (void)tmr;
    if (!s_wifi_page.scanning) return;
    s_wifi_page.scanning = false;

    ESP_LOGW(TAG, "WiFi扫描超时 (8s无SDIO事件)");
    /* 调度到 LVGL 线程中刷新UI */
    ui_lvgl_async_call((lv_async_cb_t)ui_wifi_on_scan_done, NULL);
}

/* 扫描超时定时器: 静态复用 (旧版每次扫描 xTimerCreate 且从不删除, 泄漏) */
static TimerHandle_t s_scan_to_tmr = NULL;

static void cb_scan_wifi(lv_event_t *e)
{
    (void)e;
    if (s_wifi_page.scanning) {
        ESP_LOGW(TAG, "已在扫描中, 忽略");
        return;
    }

    /* WiFi 协议栈未就绪: 按需异步启动 (worker 线程执行),
     * 启动完成事件 (WLESS_EVT_WIFI_STARTED) 会自动续接扫描 */
    if (!bsp_wireless_wifi_active()) {
        s_wifi_page.scanning = false;
        s_pending_scan = true;
        bsp_wireless_wifi_start();
        lv_obj_clean(s_wifi_page.scan_list);
        lv_obj_t *h = lv_list_add_text(s_wifi_page.scan_list, "正在启动WiFi，请稍候...");
        lv_obj_set_style_text_font(h, &esp_2, 0);
        lv_obj_set_style_text_color(h, COLOR_ACCENT, 0);
        return;
    }

    s_wifi_page.scanning = true;
    lv_obj_clean(s_wifi_page.scan_list);
    lv_obj_t *hint = lv_list_add_text(s_wifi_page.scan_list, "正在扫描...");
    lv_obj_set_style_text_font(hint, &esp_2, 0);
    lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);

    esp_err_t ret = bsp_wifi_scan();
    if (ret != ESP_OK) {
        s_wifi_page.scanning = false;
        lv_obj_clean(s_wifi_page.scan_list);
        const char *msg = "WiFi未启用";
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            msg = "未配置ESP-Hosted WiFi";
        } else if (ret == ESP_ERR_INVALID_STATE) {
            msg = "请先打开WiFi开关";
        }
        lv_obj_t *h = lv_list_add_text(s_wifi_page.scan_list, msg);
        lv_obj_set_style_text_font(h, &esp_2, 0);
        lv_obj_set_style_text_color(h, COLOR_ACCENT, 0);
        return;
    }

    /* 启动8秒超时保险 (定时器首次创建后复用) */
    if (s_scan_to_tmr == NULL) {
        s_scan_to_tmr = xTimerCreate("wifi_scan_to",
                                     pdMS_TO_TICKS(8000),
                                     pdFALSE, NULL, wifi_scan_timeout_cb);
    }
    if (s_scan_to_tmr) {
        xTimerStop(s_scan_to_tmr, 0);
        xTimerStart(s_scan_to_tmr, 0);
    }
}

/* 注册到bsp_wifi: 扫描完成时通过lv_async_call触发ui_wifi_on_scan_done */
static void wifi_scan_done_dispatcher(void)
{
    ui_lvgl_async_call((lv_async_cb_t)ui_wifi_on_scan_done, NULL);
}

/* WiFi 开关 */
static void cb_wifi_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
        /* 异步启动: worker 线程拉起 SDIO 链路 -> esp_wifi_remote,
         * 完成后经 ui_wireless_bridge 回调 ui_wifi_on_stack_event 更新 UI.
         * LVGL 线程不再被 bsp_wifi_init() 阻塞. */
        if (s_wifi_page.status_lbl) {
            lv_label_set_text(s_wifi_page.status_lbl, "正在启动WiFi...");
        }
        esp_err_t ret = bsp_wireless_wifi_start();
        if (ret != ESP_OK) {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
            if (s_wifi_page.status_lbl) {
                lv_label_set_text(s_wifi_page.status_lbl, "启动失败");
            }
        }
    } else {
        /* 异步停止: worker 会先断开连接, 再 esp_wifi_deinit 回收协议栈内存 */
        s_pending_scan = false;
        if (s_wifi_page.status_lbl) {
            lv_label_set_text(s_wifi_page.status_lbl, "正在关闭WiFi...");
        }
        bsp_wifi_disconnect();
        bsp_wireless_wifi_stop();
    }
}

/* 无线管理器事件处理: 由 ui_wireless_bridge 在 LVGL 线程中调用 */
void ui_wifi_on_stack_event(wless_event_t evt, esp_err_t err)
{
    switch (evt) {
    case WLESS_EVT_WIFI_STARTED:
        if (s_wifi_page.switch_obj) {
            lv_obj_add_state(s_wifi_page.switch_obj, LV_STATE_CHECKED);
        }
        if (s_wifi_page.status_lbl) {
            lv_label_set_text(s_wifi_page.status_lbl, "WiFi已开启");
        }
        /* 扫描请求先于协议栈就绪: 自动续接 */
        if (s_pending_scan) {
            s_pending_scan = false;
            cb_scan_wifi(NULL);
        }
        break;

    case WLESS_EVT_WIFI_START_FAIL:
        s_pending_scan = false;
        if (s_wifi_page.switch_obj) {
            lv_obj_clear_state(s_wifi_page.switch_obj, LV_STATE_CHECKED);
        }
        if (s_wifi_page.status_lbl) {
            lv_label_set_text(s_wifi_page.status_lbl,
                              (err == ESP_ERR_NOT_SUPPORTED) ? "未配置WiFi支持" :
                              (err == ESP_ERR_NO_MEM) ? "内存不足, 启动失败" : "WiFi启动失败");
        }
        break;

    case WLESS_EVT_WIFI_STOPPED:
        if (s_wifi_page.switch_obj) {
            lv_obj_clear_state(s_wifi_page.switch_obj, LV_STATE_CHECKED);
        }
        if (s_wifi_page.status_lbl) {
            lv_label_set_text(s_wifi_page.status_lbl, "WiFi未开启");
        }
        if (s_wifi_page.ip_lbl) {
            lv_label_set_text(s_wifi_page.ip_lbl, "--");
        }
        break;

    default:
        break;
    }
}

/* ================================================================
 * 创建WiFi页面
 * ================================================================ */

void refresh_wifi_page(void)
{
    if (s_wifi_page.screen == NULL) return;
    bool active = bsp_wireless_wifi_active() || bsp_wifi_is_connected();
    if (s_wifi_page.switch_obj) {
        if (active) {
            lv_obj_add_state(s_wifi_page.switch_obj, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_wifi_page.switch_obj, LV_STATE_CHECKED);
        }
    }
    if (s_wifi_page.status_lbl) {
        if (bsp_wifi_is_connected()) {
            lv_label_set_text(s_wifi_page.status_lbl, "已连接");
        } else if (active) {
            lv_label_set_text(s_wifi_page.status_lbl, "WiFi已开启");
        } else {
            lv_label_set_text(s_wifi_page.status_lbl, "WiFi未开启");
        }
    }
    if (s_wifi_page.ip_lbl) {
        const char *ip = bsp_wifi_get_ip();
        lv_label_set_text(s_wifi_page.ip_lbl, ip ? ip : "--");
    }
}

lv_obj_t *ui_wifi_create_page(void)
{
    /* 如果已存在, 直接返回 */
    if (s_wifi_page.screen) {
        refresh_wifi_page();
        return s_wifi_page.screen;
    }

    s_wifi_page.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_wifi_page.screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_wifi_page.screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_wifi_page.screen, COLOR_WHITE, 0);

    /* 侧滑返回 */
    ui_edge_swipe_add(s_wifi_page.screen, swipe_back_to_settings);

    /* 标题 */
    lv_obj_t *title = lv_label_create(s_wifi_page.screen);
    lv_label_set_text(title, "WiFi");
    lv_obj_set_style_text_font(title, &esp_2, 0);
    lv_obj_set_style_text_color(title, COLOR_WHITE, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* 侧滑提示 */
    lv_obj_t *hint_swipe = lv_label_create(s_wifi_page.screen);
    lv_label_set_text(hint_swipe, "<");
    lv_obj_set_style_text_font(hint_swipe, &esp_2, 0);
    lv_obj_set_style_text_color(hint_swipe, COLOR_GRAY, 0);
    lv_obj_align(hint_swipe, LV_ALIGN_TOP_LEFT, 8, 12);

    /* 开关行 */
    s_wifi_page.switch_obj = lv_switch_create(s_wifi_page.screen);
    lv_obj_set_size(s_wifi_page.switch_obj, 40, 22);
    lv_obj_align(s_wifi_page.switch_obj, LV_ALIGN_TOP_RIGHT, -10, 14);
    lv_obj_set_style_bg_color(s_wifi_page.switch_obj, COLOR_GRAY, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_wifi_page.switch_obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_wifi_page.switch_obj, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_wifi_page.switch_obj, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_wifi_page.switch_obj, cb_wifi_switch, LV_EVENT_VALUE_CHANGED, NULL);

    /* 状态卡片 */
    lv_obj_t *status_box = lv_obj_create(s_wifi_page.screen);
    lv_obj_set_size(status_box, SCREEN_W - 12, 56);
    lv_obj_set_pos(status_box, 6, 44);
    lv_obj_set_style_bg_color(status_box, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(status_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(status_box, 6, 0);
    lv_obj_set_style_border_width(status_box, 0, 0);
    lv_obj_set_style_pad_all(status_box, 8, 0);

    s_wifi_page.status_lbl = lv_label_create(status_box);
    lv_label_set_text(s_wifi_page.status_lbl, "未连接");
    lv_obj_set_style_text_font(s_wifi_page.status_lbl, &esp_2, 0);
    lv_obj_align(s_wifi_page.status_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    s_wifi_page.ip_lbl = lv_label_create(status_box);
    lv_label_set_text(s_wifi_page.ip_lbl, "--");
    lv_obj_set_style_text_font(s_wifi_page.ip_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(s_wifi_page.ip_lbl, COLOR_GRAY, 0);
    lv_obj_align(s_wifi_page.ip_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* 扫描按钮 */
    s_wifi_page.scan_btn = lv_btn_create(s_wifi_page.screen);
    lv_obj_set_size(s_wifi_page.scan_btn, SCREEN_W - 12, 36);
    lv_obj_set_pos(s_wifi_page.scan_btn, 6, 108);
    lv_obj_set_style_bg_color(s_wifi_page.scan_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_radius(s_wifi_page.scan_btn, 6, 0);
    lv_obj_add_event_cb(s_wifi_page.scan_btn, cb_scan_wifi, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_lbl = lv_label_create(s_wifi_page.scan_btn);
    lv_label_set_text(scan_lbl, "扫描WiFi");
    lv_obj_set_style_text_font(scan_lbl, &esp_2, 0);
    lv_obj_set_style_text_color(scan_lbl, COLOR_WHITE, 0);
    lv_obj_center(scan_lbl);

    /* 列表标题 */
    lv_obj_t *scan_title = lv_label_create(s_wifi_page.screen);
    lv_label_set_text(scan_title, "可用网络");
    lv_obj_set_style_text_font(scan_title, &esp_2, 0);
    lv_obj_set_style_text_color(scan_title, COLOR_GRAY, 0);
    lv_obj_set_pos(scan_title, 8, 152);

    /* 扫描结果列表 */
    s_wifi_page.scan_list = lv_list_create(s_wifi_page.screen);
    lv_obj_set_size(s_wifi_page.scan_list, SCREEN_W - 12,
                    SCREEN_H - 160 - TITLE_BAR_H - NAVBAR_H_WIFI);
    lv_obj_set_pos(s_wifi_page.scan_list, 6, 172);
    lv_obj_set_style_bg_color(s_wifi_page.scan_list, lv_color_hex(0x101020), 0);
    lv_obj_set_style_bg_opa(s_wifi_page.scan_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wifi_page.scan_list, 0, 0);
    lv_obj_set_style_radius(s_wifi_page.scan_list, 6, 0);
    lv_obj_set_style_pad_all(s_wifi_page.scan_list, 4, 0);
    lv_obj_set_style_text_font(s_wifi_page.scan_list, &esp_2, 0);

    lv_obj_t *hint = lv_list_add_text(s_wifi_page.scan_list, "点击上方按钮扫描");
    lv_obj_set_style_text_font(hint, &esp_2, 0);
    lv_obj_set_style_text_color(hint, COLOR_GRAY, 0);

    refresh_wifi_page();
    /* 注册扫描完成回调 (确保只注册一次) */
    bsp_wifi_set_scan_done_cb(wifi_scan_done_dispatcher);
    return s_wifi_page.screen;
}

lv_obj_t *ui_wifi_get_screen(void)
{
    return s_wifi_page.screen;
}

/* 主题切换时由 ui_settings 调用, 销毁旧页面, 下次访问时重建 */
void ui_wifi_invalidate(void)
{
    if (s_wifi_page.screen) {
        lv_obj_del(s_wifi_page.screen);
        s_wifi_page.screen = NULL;
        memset(&s_wifi_page, 0, sizeof(s_wifi_page));
    }
    if (s_pwd_dlg.screen) {
        lv_obj_del(s_pwd_dlg.screen);
        s_pwd_dlg.screen = NULL;
        memset(&s_pwd_dlg, 0, sizeof(s_pwd_dlg));
    }
    /* 注销扫描完成回调 */
    bsp_wifi_set_scan_done_cb(NULL);
}
