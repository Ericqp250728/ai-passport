// tools/badge_preview/preview.c —— 在电脑上用真实的 badge_ui.c 渲染胸牌各页面,输出 PPM 截图。
// 由 tools/badge_preview.py 编译运行;不参与固件构建。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "badge_content.h"
#include "badge_ledger.h"
#include "badge_nav.h"
#include "badge_ui.h"
#include "bsp_display_rounding.h"
#include "lvgl.h"

#define W 240
#define H 320
#define RADIUS 30   // 与 BSP_LVGL_SCREEN_RADIUS 一致

static uint16_t s_fb[W * H];
static uint32_t s_tick;

static uint32_t tick_cb(void) { return s_tick; }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
    const uint16_t *src = (const uint16_t *)px;
    int32_t w = lv_area_get_width(area);
    for (int32_t y = area->y1; y <= area->y2; y++) {
        memcpy(&s_fb[y * W + area->x1], &src[(y - area->y1) * w], (size_t)w * 2);
    }
    lv_display_flush_ready(disp);
}

static void render(void) {
    // 让提示浮层等定时器有机会运行,再强制整屏重绘
    lv_timer_handler();
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(NULL);
}

static int save(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t rgb[3] = { 0, 0, 0 };
            // 与设备一致:圆角外的像素由 BSP 遮成纯黑
            if (!bsp_display_pixel_outside_rounded_rect(x, y, W, H, RADIUS)) {
                uint16_t c = s_fb[y * W + x];
                rgb[0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
                rgb[1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
                rgb[2] = (uint8_t)((c & 0x1F) * 255 / 31);
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("%-14s LVGL pool used %3u%%, biggest free block %u bytes\n",
           name, mon.used_pct, (unsigned)mon.free_biggest_size);
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    static uint16_t draw_buf[W * 40];   // 与设备相同的 40 行局部缓冲
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    size_t items = BADGE_CONTENT.item_count < BADGE_MAX_ITEMS ? BADGE_CONTENT.item_count : BADGE_MAX_ITEMS;
    size_t pays = BADGE_CONTENT.pay_count < BADGE_MAX_PAY ? BADGE_CONTENT.pay_count : BADGE_MAX_PAY;
    badge_nav_t nav;
    badge_nav_init(&nav, pays, items, 0);

    // 一场模拟营业,让记账页和账本页有内容
    badge_ledger_t ledger;
    badge_ledger_reset(&ledger);
    static const size_t sales[] = { 0, 1, 0, 2, 3, 0, 1, 4, 2, 0, 1 };
    for (size_t i = 0; i < sizeof(sales) / sizeof(sales[0]); i++) {
        size_t item = items ? sales[i] % items : 0;
        if (items) badge_ledger_record(&ledger, item, BADGE_CONTENT.items[item].price_cents);
    }

    badge_ui_init(&BADGE_CONTENT);
    badge_ui_set_battery(78);

    nav.page = BADGE_PAGE_SIGN;
    badge_ui_show_page(&nav, &ledger);
    render();
    save(dir, "1-sign");

    nav.page = BADGE_PAGE_PAY;
    badge_ui_show_page(&nav, &ledger);
    render();
    save(dir, "2-pay");

    nav.page = BADGE_PAGE_SELL;
    nav.sell_index = items > 1 ? 1 : 0;
    badge_ui_show_page(&nav, &ledger);
    if (items) badge_ui_toast("+\xC2\xA5" "35  \xE6\x89\x8B\xE9\x93\xBE", false);   // "+¥35  手链"
    render();
    save(dir, "3-sell");

    s_tick += 2000;   // 让提示浮层到时消失
    nav.page = BADGE_PAGE_LEDGER;
    nav.clear_armed = true;
    badge_ui_show_page(&nav, &ledger);
    render();
    save(dir, "4-ledger");
    return 0;
}
