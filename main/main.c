// main/main.c —— 摊主胸牌:挂在胸前的招牌、收款码和本场记账本。
//
// 线程模型:
//   按键回调(esp_timer 任务) ──入队──▶ badge_app 任务(本文件)──持锁──▶ LVGL
//                                          │
//                                          ├─ NVS 保存账本(阻塞数毫秒,不在回调/LVGL 锁里做)
//                                          ├─ 每 30 秒读一次电量(I2C)
//                                          └─ 提示音入队 ──▶ badge_sfx 任务 ──▶ I2S
// 按键与页面的对应关系见 badge_nav.h。
#include "badge_content.h"
#include "badge_fmt.h"
#include "badge_ledger.h"
#include "badge_nav.h"
#include "badge_sfx.h"
#include "badge_store.h"
#include "badge_ui.h"
#include "bsp_audio.h"
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
#include <stdio.h>

static const char *TAG = "badge";

#define INPUT_QUEUE_DEPTH   8
#define TICK_MS             500
#define BATTERY_PERIOD_MS   30000u

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static badge_nav_t s_nav;
static badge_ledger_t s_ledger;
static bool s_battery_ok;
static uint32_t s_last_battery_ms;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void apply_backlight(void) {
    bsp_display_backlight(badge_nav_backlight(&s_nav));
}

static void toast(const char *text, bool error) {
    if (!bsp_lvgl_lock(500)) return;
    badge_ui_toast(text, error);
    badge_ui_refresh(&s_nav, &s_ledger);
    bsp_lvgl_unlock();
}

static bool save_ledger(void) {
    esp_err_t e = badge_store_save_ledger(&s_ledger);
    if (e != ESP_OK) ESP_LOGE(TAG, "账本保存失败: %s", esp_err_to_name(e));
    return e == ESP_OK;
}

static void do_record(void) {
    size_t item = s_nav.sell_index;
    const badge_item_t *it = &BADGE_CONTENT.items[item];
    if (!badge_ledger_record(&s_ledger, item, it->price_cents)) {
        badge_sfx_play(BADGE_SFX_ERROR);
        toast(it->price_cents == 0 ? "这件商品还没设价格" : "记不下了,先清零账本", true);
        return;
    }
    bool saved = save_ledger();
    badge_sfx_play(BADGE_SFX_RECORD);
    char price[24], msg[64];
    badge_fmt_cents(it->price_cents, true, price, sizeof(price));
    if (saved) {
        snprintf(msg, sizeof(msg), "+%s  %s", price, it->name);
    } else {
        snprintf(msg, sizeof(msg), "+%s 已记(未存盘)", price);
    }
    toast(msg, !saved);
}

static void do_undo(void) {
    size_t item;
    uint32_t cents;
    if (!badge_ledger_undo(&s_ledger, &item, &cents)) {
        badge_sfx_play(BADGE_SFX_ERROR);
        toast("没有可撤销的记录", true);
        return;
    }
    bool saved = save_ledger();
    badge_sfx_play(BADGE_SFX_UNDO);
    char price[24], msg[64];
    badge_fmt_cents(cents, true, price, sizeof(price));
    snprintf(msg, sizeof(msg), "已撤销 %s -%s", BADGE_CONTENT.items[item].name, price);
    toast(msg, !saved);
}

static void do_clear(void) {
    badge_ledger_reset(&s_ledger);
    bool saved = save_ledger();
    badge_sfx_play(BADGE_SFX_CLEAR);
    toast(saved ? "本场账本已清零" : "已清零(未存盘)", !saved);
}

static void handle(badge_action_t action, bool from_user) {
    switch (action) {
    case BADGE_ACTION_NONE:
        return;
    case BADGE_ACTION_PAGE:
        if (bsp_lvgl_lock(500)) {
            badge_ui_show_page(&s_nav, &s_ledger);
            bsp_lvgl_unlock();
        }
        break;
    case BADGE_ACTION_SELECT:
    case BADGE_ACTION_CLEAR_ARM:
    case BADGE_ACTION_CLEAR_DISARM:
        if (bsp_lvgl_lock(500)) {
            badge_ui_refresh(&s_nav, &s_ledger);
            bsp_lvgl_unlock();
        }
        break;
    case BADGE_ACTION_BRIGHTNESS:
        // 只有用户主动调节才存盘;闲置调暗是临时状态
        if (from_user) (void)badge_store_save_brightness(s_nav.brightness);
        break;
    case BADGE_ACTION_WAKE:
        break;
    case BADGE_ACTION_RECORD:
        do_record();
        break;
    case BADGE_ACTION_UNDO:
        do_undo();
        break;
    case BADGE_ACTION_CLEAR:
        do_clear();
        break;
    }
    apply_backlight();
}

static void on_input(const input_event_t *input) {
    badge_key_t key = input->btn == BSP_BTN_UP ? BADGE_KEY_UP
                    : input->btn == BSP_BTN_DOWN ? BADGE_KEY_DOWN : BADGE_KEY_OK;
    switch (input->event) {
    case BSP_BTN_CLICK:
        handle(badge_nav_input(&s_nav, key, BADGE_PRESS_CLICK, now_ms()), true);
        break;
    case BSP_BTN_DOUBLE:
        // 快速连按两下时 button 组件只报 DOUBLE,不报两次 CLICK;按两次单击处理,
        // 否则在价目表里快速下移会"丢一格"。
        handle(badge_nav_input(&s_nav, key, BADGE_PRESS_CLICK, now_ms()), true);
        handle(badge_nav_input(&s_nav, key, BADGE_PRESS_CLICK, now_ms()), true);
        break;
    case BSP_BTN_LONG:
        handle(badge_nav_input(&s_nav, key, BADGE_PRESS_LONG, now_ms()), true);
        break;
    default:
        break;   // PRESS(按下瞬间)不用:本应用都是"松手/长按"语义
    }
}

static void refresh_battery(void) {
    if (!s_battery_ok) return;
    int soc = bsp_battery_soc();   // I2C 读,放在锁外
    if (bsp_lvgl_lock(500)) {
        badge_ui_set_battery(soc);
        bsp_lvgl_unlock();
    }
}

static void app_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            on_input(&input);
        }
        uint32_t now = now_ms();
        handle(badge_nav_tick(&s_nav, now), false);
        if ((uint32_t)(now - s_last_battery_ms) >= BATTERY_PERIOD_MS) {
            s_last_battery_ms = now;
            refresh_battery();
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
    ESP_LOGI(TAG, "摊主胸牌启动");

    bsp_i2c_init();

    // 屏幕是胸牌的全部意义:失败就打清楚日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    if (badge_store_init() == ESP_OK) {
        esp_err_t e = badge_store_load_ledger(&s_ledger);
        if (e != ESP_OK) ESP_LOGW(TAG, "账本读取: %s", esp_err_to_name(e));
    } else {
        badge_ledger_reset(&s_ledger);
    }

    size_t item_count = BADGE_CONTENT.item_count;
    if (item_count > BADGE_MAX_ITEMS) item_count = BADGE_MAX_ITEMS;
    size_t pay_count = BADGE_CONTENT.pay_count;
    if (pay_count > BADGE_MAX_PAY) pay_count = BADGE_MAX_PAY;
    badge_nav_init(&s_nav, pay_count, item_count, now_ms());
    uint8_t level = badge_store_load_brightness(s_nav.brightness);
    if (level < BADGE_BRIGHTNESS_LEVELS) s_nav.brightness = level;

    // 声音和电量是锦上添花:失败只记日志,胸牌照常工作。
    if (bsp_audio_init() == ESP_OK) {
        if (badge_sfx_init() != ESP_OK) ESP_LOGW(TAG, "提示音任务创建失败,将静音运行");
    } else {
        ESP_LOGW(TAG, "音频初始化失败,将静音运行");
    }
    s_battery_ok = bsp_battery_init() == ESP_OK;
    int soc = s_battery_ok ? bsp_battery_soc() : -1;
    s_last_battery_ms = now_ms();

    if (bsp_lvgl_lock(1000)) {
        badge_ui_init(&BADGE_CONTENT);
        badge_ui_set_battery(soc);
        badge_ui_show_page(&s_nav, &s_ledger);
        bsp_lvgl_unlock();
    }
    apply_backlight();

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue || xTaskCreate(app_task, "badge_app", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "应用任务创建失败,胸牌只能显示招牌");
        return;
    }
    esp_err_t e = bsp_button_init(on_key, NULL);
    if (e != ESP_OK) ESP_LOGE(TAG, "按键初始化失败: %s,胸牌只能显示招牌", esp_err_to_name(e));

    ESP_LOGI(TAG, "就绪:本场 %u 笔,%lu 分;商品 %u 种,收款码 %u 个",
             (unsigned)s_ledger.sales, (unsigned long)s_ledger.total_cents,
             (unsigned)item_count, (unsigned)pay_count);
}
