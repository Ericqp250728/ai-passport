// tools/mirror_preview/preview.c —— 在电脑上用真实的 mirror_ui.c 渲染各页面,输出 PPM 截图。
// 由 tools/mirror_preview.py 编译运行;不参与固件构建。示例通知是虚构的。
#include <stdio.h>
#include <string.h>

#include "bsp_display_rounding.h"
#include "lvgl.h"
#include "mirror_nav.h"
#include "mirror_ui.h"
#include "notif_model.h"

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
    lv_timer_handler();
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(NULL);
}

static void save(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t rgb[3] = { 0, 0, 0 };
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
    printf("%-12s LVGL pool used %3u%%, biggest free block %u bytes\n",
           name, mon.used_pct, (unsigned)mon.free_biggest_size);
}

static void add(notif_list_t *l, uint32_t uid, const char *app, uint8_t cat, const char *title,
                const char *sub, const char *msg, uint32_t at) {
    ancs_attrs_t a;
    memset(&a, 0, sizeof(a));
    a.uid = uid;
    snprintf(a.app_id, sizeof(a.app_id), "%s", app);
    snprintf(a.title, sizeof(a.title), "%s", title);
    snprintf(a.subtitle, sizeof(a.subtitle), "%s", sub);
    snprintf(a.message, sizeof(a.message), "%s", msg);
    notif_list_upsert(l, &a, cat, at);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    static uint16_t draw_buf[W * 40];
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    uint32_t now = 10 * 60000;
    notif_list_t list;
    notif_list_init(&list);
    add(&list, 1, "jp.naver.line", ANCS_CAT_SOCIAL, "ホテル新宿", "",
        "チェックインは15時からです。お気をつけてお越しください。", now - 9 * 60000);
    add(&list, 2, "com.apple.MobileSMS", ANCS_CAT_SOCIAL, "+81 90-1234-5678", "",
        "予約確認:明日 19:00 2名様", now - 6 * 60000);
    add(&list, 3, "com.tencent.xin", ANCS_CAT_SOCIAL, "妈妈", "",
        "到酒店了吗?记得吃饭,别太累 \xF0\x9F\x98\x8A", now - 2 * 60000);
    add(&list, 4, "com.google.Maps", ANCS_CAT_LOCATION, "300 m 后右转",
        "", "进入 明治通り,前往 新宿駅 東口", now - 20000);
    add(&list, 5, "com.tencent.xin", ANCS_CAT_SOCIAL, "小林",
        "", "我们在 新宿駅 东口的 ALTA 门口集合,大概 7 点半到。到了给我发消息,"
            "晚饭去思い出横丁吃烤串,吃完再去 歌舞伎町 逛逛。", now - 5000);

    mirror_nav_t nav;
    mirror_nav_init(&nav, now);
    mirror_nav_set_count(&nav, list.count, true);

    mirror_ui_init("AI Passport");
    mirror_ui_set_battery(76);

    nav.page = MIRROR_PAGE_NAV;
    mirror_ui_show(&nav, &list, MIRROR_LINK_READY, now);
    render();
    save(dir, "1-nav");

    nav.page = MIRROR_PAGE_INBOX;
    nav.selected = 1;
    mirror_ui_show(&nav, &list, MIRROR_LINK_READY, now);
    render();
    save(dir, "2-inbox");

    nav.selected = 0;
    nav.detail = true;
    mirror_ui_show(&nav, &list, MIRROR_LINK_READY, now);
    render();
    save(dir, "3-detail");

    nav.detail = false;
    nav.page = MIRROR_PAGE_NAV;
    mirror_ui_show(&nav, &list, MIRROR_LINK_READY, now);
    mirror_ui_banner(&list.items[0]);
    render();
    save(dir, "4-banner");

    s_tick += 6000;   // 横幅收起
    nav.page = MIRROR_PAGE_STATUS;
    mirror_ui_show(&nav, &list, MIRROR_LINK_ADVERTISING, now);
    render();
    save(dir, "5-pairing");
    return 0;
}
