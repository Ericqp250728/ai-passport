// main/main.c —— 通知镜像:把 iPhone 的导航提示和消息显示在胸前。
//
// 线程模型:
//   按键回调(esp_timer 任务) ──入队──┐
//   NimBLE host 任务(mirror_ble)──入队──┴──▶ mirror_app 任务(本文件)──持锁──▶ LVGL
// 应用任务独占 nav 状态与通知列表;每 0.5 秒处理亮屏超时,每 30 秒读电量、刷新"几分钟前"。
// 页面与按键见 mirror_nav.h。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mirror_ble.h"
#include "mirror_nav.h"
#include "mirror_ui.h"
#include "notif_model.h"
#include "nvs_flash.h"

static const char *TAG = "mirror";
static const char *DEVICE_NAME = "AI Passport";

#define INPUT_QUEUE_DEPTH  8
#define BLE_QUEUE_DEPTH    6
#define TICK_MS            100
#define SLOW_PERIOD_MS     30000u

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static QueueHandle_t s_ble_queue;
static mirror_nav_t s_nav;
static notif_list_t s_list;
static mirror_link_t s_link = MIRROR_LINK_OFF;
static bool s_battery_ok;
static uint8_t s_applied_backlight = 255;
static mirror_event_t s_ev;   // 较大,放静态区

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void apply_backlight(void) {
    if (s_nav.backlight == s_applied_backlight) return;
    s_applied_backlight = s_nav.backlight;
    bsp_display_backlight(s_nav.backlight);
}

static void show(void) {
    if (!bsp_lvgl_lock(500)) return;
    mirror_ui_show(&s_nav, &s_list, s_link, now_ms());
    bsp_lvgl_unlock();
}

static void refresh(void) {
    if (!bsp_lvgl_lock(500)) return;
    if (s_nav.detail) {
        uint8_t pages = mirror_ui_detail_pages();
        if (s_nav.detail_page >= pages) s_nav.detail_page = (uint8_t)(pages - 1);
    }
    mirror_ui_refresh(&s_nav, &s_list, s_link, now_ms());
    bsp_lvgl_unlock();
}

static void handle(mirror_act_t act) {
    switch (act) {
    case MIRROR_ACT_PAGE:
        show();
        break;
    case MIRROR_ACT_SELECT:
    case MIRROR_ACT_FORGET_ARM:
    case MIRROR_ACT_FORGET_DISARM:
        refresh();
        break;
    case MIRROR_ACT_FORGET:
        mirror_ble_forget();
        notif_list_init(&s_list);
        mirror_nav_set_count(&s_nav, 0, false);
        show();
        break;
    default:
        break;
    }
    apply_backlight();
}

// 当前页面是否直接展示了通知列表的内容(列表变化时需要整页重建)
static bool page_shows_list(void) {
    return s_nav.detail || s_nav.page == MIRROR_PAGE_INBOX || s_nav.page == MIRROR_PAGE_NAV;
}

static void on_ble_event(const mirror_event_t *ev) {
    uint32_t now = now_ms();
    switch (ev->type) {
    case MIRROR_EV_LINK: {
        bool changed = ev->link != s_link;
        s_link = ev->link;
        ESP_LOGI(TAG, "连接状态 %d", s_link);
        if (!changed) return;
        if (s_nav.page == MIRROR_PAGE_STATUS && !s_nav.detail) show();
        else refresh();
        return;
    }
    case MIRROR_EV_NOTIF: {
        const notif_t *n = notif_list_upsert(&s_list, &ev->attrs, ev->category, now);
        mirror_nav_set_count(&s_nav, s_list.count, true);
        if (ev->pre_existing) {
            // 补发的旧通知:只更新列表
            if (page_shows_list() && !s_nav.detail) show();
            return;
        }
        mirror_act_t act = mirror_nav_on_notif(&s_nav, n->kind == NOTIF_KIND_NAV, now);
        if (act == MIRROR_ACT_PAGE || (page_shows_list() && !s_nav.detail)) show();
        if (n->kind != NOTIF_KIND_NAV && bsp_lvgl_lock(500)) {
            mirror_ui_banner(n);
            bsp_lvgl_unlock();
        }
        apply_backlight();
        return;
    }
    case MIRROR_EV_REMOVED: {
        bool viewing = s_nav.detail && s_nav.selected < s_list.count
                    && s_list.items[s_nav.selected].uid == ev->uid;
        if (!notif_list_remove(&s_list, ev->uid)) return;
        mirror_act_t act = mirror_nav_set_count(&s_nav, s_list.count, !viewing);
        if (act == MIRROR_ACT_PAGE || (page_shows_list() && !s_nav.detail)) show();
        return;
    }
    }
}

static void on_input(const input_event_t *in) {
    mirror_key_t key = in->btn == BSP_BTN_UP ? MIRROR_KEY_UP
                     : in->btn == BSP_BTN_DOWN ? MIRROR_KEY_DOWN : MIRROR_KEY_OK;
    uint32_t now = now_ms();
    switch (in->event) {
    case BSP_BTN_CLICK:
        handle(mirror_nav_input(&s_nav, key, MIRROR_PRESS_CLICK, now));
        break;
    case BSP_BTN_DOUBLE:
        // 快速连按两下时 button 组件只报 DOUBLE:按两次单击处理
        handle(mirror_nav_input(&s_nav, key, MIRROR_PRESS_CLICK, now));
        handle(mirror_nav_input(&s_nav, key, MIRROR_PRESS_CLICK, now));
        break;
    case BSP_BTN_LONG:
        handle(mirror_nav_input(&s_nav, key, MIRROR_PRESS_LONG, now));
        break;
    default:
        break;
    }
}

static void slow_tick(void) {
    if (s_battery_ok) {
        int soc = bsp_battery_soc();   // I2C 读,放在锁外
        if (bsp_lvgl_lock(500)) {
            mirror_ui_set_battery(soc);
            bsp_lvgl_unlock();
        }
    }
    if (s_nav.page == MIRROR_PAGE_NAV && !s_nav.detail) refresh();   // "几分钟前"
}

static void app_task(void *arg) {
    (void)arg;
    input_event_t input;
    uint32_t last_slow = now_ms();
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            on_input(&input);
        }
        while (xQueueReceive(s_ble_queue, &s_ev, 0) == pdTRUE) on_ble_event(&s_ev);
        uint32_t now = now_ms();
        handle(mirror_nav_tick(&s_nav, now));
        if ((uint32_t)(now - last_slow) >= SLOW_PERIOD_MS) {
            last_slow = now;
            slow_tick();
        }
    }
}

// 按键回调运行在共享的 esp_timer 任务里:只入队,立即返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "通知镜像启动");
    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    // NVS 保存蓝牙配对与射频校准数据。初始化失败时不擦除(那会清掉别的数据),
    // 蓝牙照常工作,只是每次开机都要重新配对。
    esp_err_t e = nvs_flash_init();
    if (e != ESP_OK) ESP_LOGE(TAG, "NVS 初始化失败(%s),配对不会保存", esp_err_to_name(e));

    mirror_nav_init(&s_nav, now_ms());
    notif_list_init(&s_list);
    s_battery_ok = bsp_battery_init() == ESP_OK;
    int soc = s_battery_ok ? bsp_battery_soc() : -1;

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    s_ble_queue = xQueueCreate(BLE_QUEUE_DEPTH, sizeof(mirror_event_t));
    if (!s_input_queue || !s_ble_queue) {
        ESP_LOGE(TAG, "队列创建失败");
        return;
    }

    e = mirror_ble_start(s_ble_queue, DEVICE_NAME);
    if (e != ESP_OK) ESP_LOGE(TAG, "蓝牙启动失败: %s", esp_err_to_name(e));
    else s_link = MIRROR_LINK_ADVERTISING;
    // 启动失败或还没配过对:开机先显示连接引导页
    if (e != ESP_OK || !mirror_ble_has_bond()) s_nav.page = MIRROR_PAGE_STATUS;

    if (bsp_lvgl_lock(1000)) {
        mirror_ui_init(DEVICE_NAME);
        mirror_ui_set_battery(soc);
        mirror_ui_show(&s_nav, &s_list, s_link, now_ms());
        bsp_lvgl_unlock();
    }
    apply_backlight();

    if (xTaskCreate(app_task, "mirror_app", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "应用任务创建失败");
        return;
    }
    e = bsp_button_init(on_key, NULL);
    if (e != ESP_OK) ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(e));
    ESP_LOGI(TAG, "就绪,空闲堆 %u 字节", (unsigned)esp_get_free_heap_size());
}
