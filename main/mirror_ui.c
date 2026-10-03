// main/mirror_ui.c —— 深色界面:导航大字卡、消息列表与详情、连接引导页。
//
// 屏幕 240x320 竖屏,四角被 BSP 以 30px 圆角遮黑,内容离角落留余量。
// 顶部状态栏(页名、页码点、蓝牙状态、电量)与通知横幅放在 lv_layer_top(),切页不重建。
// 通知文字是动态内容:字库覆盖常用中文、日文假名与常用汉字(见 tools/gen_mirror_fonts.py),
// 表情等符号在 notif_sanitize() 中去掉,其余未覆盖的字会显示为占位方框。
#include "mirror_ui.h"

#include "lvgl.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(mirror_font_16)
LV_FONT_DECLARE(mirror_font_24)

#define SCR_W 240
#define SCR_H 320

#define C_BG      0x0D1117
#define C_PANEL   0x161B22
#define C_LINE    0x262C36
#define C_TEXT    0xE6EDF3
#define C_MUTED   0x8B949E
#define C_TEAL    0x2DD4BF   // 导航
#define C_BLUE    0x58A6FF   // 消息
#define C_AMBER   0xF0B429   // 来电、进行中
#define C_RED     0xF85149
#define C_GREEN   0x3FB950

#define INBOX_ROWS   5
#define INBOX_ROW_H  52
#define DETAIL_TOP   112
#define DETAIL_H     176
#define DETAIL_STEP  160         // 翻一页移动的像素,留 16px 重叠方便接着读
#define BANNER_MS    5000

static const char *s_device_name;
static lv_obj_t *s_page;
static lv_obj_t *s_title;
static lv_obj_t *s_dots[MIRROR_PAGE_COUNT];
static lv_obj_t *s_link_dot;
static lv_obj_t *s_battery;
static lv_obj_t *s_banner;
static lv_obj_t *s_banner_app;
static lv_obj_t *s_banner_title;
static lv_obj_t *s_banner_body;
static lv_timer_t *s_banner_timer;

// 各页需要局部刷新的对象;整页重建时清空。
static struct {
    lv_obj_t *nav_age;
    lv_obj_t *rows[INBOX_ROWS];
    lv_obj_t *detail_body;
    lv_obj_t *detail_more;
    lv_obj_t *forget_hint;
    uint8_t detail_pages;
} s_w;

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *str) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, str);
    return l;
}

// 固定宽度、限定行数,超出末尾显示省略号。
static lv_obj_t *lines(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *str,
                       int x, int y, int w, int max_lines) {
    lv_obj_t *l = text(parent, font, color, str);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, lv_font_get_line_height(font) * max_lines);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    return l;
}

static uint32_t kind_color(notif_kind_t kind) {
    return kind == NOTIF_KIND_NAV ? C_TEAL : kind == NOTIF_KIND_CALL ? C_AMBER : C_BLUE;
}

// 小圆角标签:App 名
static lv_obj_t *chip(lv_obj_t *parent, const char *label, uint32_t color, int x, int y) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_20, 0);
    lv_obj_set_style_pad_hor(c, 8, 0);
    lv_obj_set_style_pad_ver(c, 1, 0);
    text(c, &mirror_font_16, color, label);
    return c;
}

// ---------------------------------------------------------------- 顶部状态栏与横幅

static const char *const PAGE_NAMES[MIRROR_PAGE_COUNT] = { "导航", "消息", "连接" };

static void top_bar(mirror_page_t page, bool detail, mirror_link_t link) {
    lv_label_set_text(s_title, detail ? "消息详情" : PAGE_NAMES[page]);
    int x = 102;
    for (int i = 0; i < MIRROR_PAGE_COUNT; i++) {
        bool cur = (mirror_page_t)i == page;
        int w = cur ? 14 : 5;
        lv_obj_set_pos(s_dots[i], x, 15);
        lv_obj_set_width(s_dots[i], w);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(cur ? C_TEXT : C_LINE), 0);
        x += w + 4;
    }
    uint32_t c = link == MIRROR_LINK_READY ? C_GREEN
               : link == MIRROR_LINK_OFF || link == MIRROR_LINK_NO_ANCS ? C_RED
               : link == MIRROR_LINK_ADVERTISING ? C_MUTED : C_AMBER;
    lv_obj_set_style_bg_color(s_link_dot, lv_color_hex(c), 0);
}

static void banner_hide_cb(lv_timer_t *t) {
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(t);
}

void mirror_ui_init(const char *device_name) {
    s_device_name = device_name;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *top = lv_layer_top();
    s_title = text(top, &mirror_font_16, C_TEXT, "");
    lv_obj_set_pos(s_title, 30, 6);
    for (int i = 0; i < MIRROR_PAGE_COUNT; i++) {
        s_dots[i] = box(top, 0, 15, 5, 5, C_LINE);
        lv_obj_set_style_radius(s_dots[i], 3, 0);
    }
    s_link_dot = box(top, 150, 13, 8, 8, C_MUTED);
    lv_obj_set_style_radius(s_link_dot, 4, 0);
    s_battery = text(top, &mirror_font_16, C_MUTED, "");
    lv_obj_set_width(s_battery, 44);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_battery, 162, 6);
    lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);

    s_banner = box(top, 10, 34, SCR_W - 20, 92, C_PANEL);
    lv_obj_set_style_radius(s_banner, 14, 0);
    lv_obj_set_style_border_width(s_banner, 2, 0);
    lv_obj_set_style_border_color(s_banner, lv_color_hex(C_BLUE), 0);
    s_banner_app = text(s_banner, &mirror_font_16, C_BLUE, "");
    lv_obj_set_pos(s_banner_app, 12, 6);
    s_banner_title = lines(s_banner, &mirror_font_16, C_TEXT, "", 12, 30, SCR_W - 44, 1);
    s_banner_body = lines(s_banner, &mirror_font_16, C_MUTED, "", 12, 54, SCR_W - 44, 1);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    s_banner_timer = lv_timer_create(banner_hide_cb, BANNER_MS, NULL);
    lv_timer_pause(s_banner_timer);
}

void mirror_ui_set_battery(int soc) {
    if (soc < 0) {
        lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_battery, "%d%%", soc > 100 ? 100 : soc);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(soc <= 15 ? C_RED : C_MUTED), 0);
}

void mirror_ui_banner(const notif_t *n) {
    uint32_t c = kind_color(n->kind);
    lv_obj_set_style_border_color(s_banner, lv_color_hex(c), 0);
    lv_obj_set_style_text_color(s_banner_app, lv_color_hex(c), 0);
    lv_label_set_text(s_banner_app, n->kind == NOTIF_KIND_CALL ? "来电" : n->app);
    lv_label_set_text(s_banner_title, n->title);
    lv_label_set_text(s_banner_body, n->body);
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_banner);
    lv_timer_reset(s_banner_timer);
    lv_timer_resume(s_banner_timer);
}

// ---------------------------------------------------------------- 导航页

#define ARROW_BOX 64
#define CARD_H    252

// 箭头用折线画:不占字库,任意大小都清晰。坐标在 64x64 的框内。
static const lv_point_precise_t ARROW_STRAIGHT[][3] = {
    { { 32, 60 }, { 32, 8 }, { 32, 8 } }, { { 12, 28 }, { 32, 8 }, { 52, 28 } } };
static const lv_point_precise_t ARROW_RIGHT[][3] = {
    { { 18, 60 }, { 18, 30 }, { 54, 30 } }, { { 38, 14 }, { 54, 30 }, { 38, 46 } } };
static const lv_point_precise_t ARROW_LEFT[][3] = {
    { { 46, 60 }, { 46, 30 }, { 10, 30 } }, { { 26, 14 }, { 10, 30 }, { 26, 46 } } };
static const lv_point_precise_t ARROW_UTURN[][3] = {
    { { 46, 60 }, { 46, 12 }, { 18, 12 } }, { { 4, 34 }, { 18, 48 }, { 32, 34 } } };
static const lv_point_precise_t UTURN_STEM[] = { { 18, 12 }, { 18, 46 } };

static void polyline(lv_obj_t *parent, const lv_point_precise_t *pts, uint32_t n, int x, int y) {
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, pts, n);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_line_width(l, 9, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(C_TEAL), 0);
}

static bool draw_arrow(lv_obj_t *parent, notif_arrow_t arrow, int x, int y) {
    const lv_point_precise_t (*a)[3] = NULL;
    switch (arrow) {
    case NOTIF_ARROW_STRAIGHT: a = ARROW_STRAIGHT; break;
    case NOTIF_ARROW_RIGHT:    a = ARROW_RIGHT; break;
    case NOTIF_ARROW_LEFT:     a = ARROW_LEFT; break;
    case NOTIF_ARROW_UTURN:    a = ARROW_UTURN; break;
    default:                   return false;
    }
    polyline(parent, a[0], 3, x, y);
    polyline(parent, a[1], 3, x, y);
    if (arrow == NOTIF_ARROW_UTURN) polyline(parent, UTURN_STEM, 2, x, y);
    return true;
}

static void build_nav(lv_obj_t *page, const notif_list_t *list, uint32_t now_ms) {
    const notif_t *n = notif_list_latest_nav(list);
    if (!n) {
        lv_obj_t *box_ = box(page, (SCR_W - ARROW_BOX) / 2, 62, ARROW_BOX, ARROW_BOX, C_BG);
        draw_arrow(box_, NOTIF_ARROW_STRAIGHT, 0, 0);
        lv_obj_t *t = text(page, &mirror_font_24, C_TEXT, "还没有导航");
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 136);
        lv_obj_t *h = text(page, &mirror_font_16, C_MUTED,
                           "在 iPhone 上用谷歌地图\n或苹果地图开始导航,\n转弯提示会显示在这里");
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 176);
        return;
    }
    lv_obj_t *card = box(page, 12, 36, SCR_W - 24, CARD_H, C_PANEL);
    lv_obj_set_style_radius(card, 16, 0);
    box(card, 0, 0, 6, CARD_H, C_TEAL);
    chip(card, n->app, C_TEAL, 18, 12);

    // 箭头根据标题+正文的关键词判断;认不出方向就不画
    char both[NOTIF_TITLE_SIZE + NOTIF_BODY_SIZE + 1];
    snprintf(both, sizeof(both), "%s %s", n->title, n->body);
    int y = 46;
    if (draw_arrow(card, notif_nav_arrow(both), (SCR_W - 24 - ARROW_BOX) / 2, 40)) y = 112;

    // 标题按内容高度排版(最多 3 行),正文紧跟其后,用满卡片剩余空间
    int w = SCR_W - 24 - 30;
    lv_obj_t *title = text(card, &mirror_font_24, C_TEXT, n->title);
    lv_obj_set_pos(title, 18, y);
    lv_obj_set_width(title, w);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_update_layout(title);
    int line24 = lv_font_get_line_height(&mirror_font_24);
    int th = lv_obj_get_height(title);
    if (th > line24 * 3) {
        th = line24 * 3;
        lv_obj_set_height(title, th);
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    }
    int by = y + th + 8;
    int line16 = lv_font_get_line_height(&mirror_font_16);
    int body_lines = (CARD_H - 12 - by) / line16;
    if (body_lines > 0 && n->body[0]) lines(card, &mirror_font_16, C_MUTED, n->body, 18, by, w, body_lines);

    s_w.nav_age = text(page, &mirror_font_16, C_MUTED, "");
    lv_obj_align(s_w.nav_age, LV_ALIGN_TOP_MID, 0, 292);
    char age[24];
    notif_fmt_age(now_ms - n->at_ms, age, sizeof(age));
    lv_label_set_text_fmt(s_w.nav_age, "%s更新", age);
}

// ---------------------------------------------------------------- 消息页

static size_t inbox_first(size_t selected, size_t count) {
    if (count <= INBOX_ROWS || selected < INBOX_ROWS / 2) return 0;
    size_t first = selected - INBOX_ROWS / 2;
    return first + INBOX_ROWS > count ? count - INBOX_ROWS : first;
}

static void refresh_inbox(const mirror_nav_t *nav, const notif_list_t *list) {
    size_t first = inbox_first(nav->selected, list->count);
    for (size_t r = 0; r < INBOX_ROWS; r++) {
        lv_obj_t *row = s_w.rows[r];
        size_t i = first + r;
        if (i >= list->count) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const notif_t *n = &list->items[i];
        bool sel = i == nav->selected;
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(row, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_t *app = lv_obj_get_child(row, 0);
        lv_obj_t *title = lv_obj_get_child(row, 1);
        lv_obj_t *body = lv_obj_get_child(row, 2);
        lv_label_set_text(app, n->kind == NOTIF_KIND_CALL ? "来电" : n->app);
        lv_obj_set_style_text_color(app, lv_color_hex(kind_color(n->kind)), 0);
        lv_label_set_text(title, n->title);
        lv_label_set_text(body, n->body);
    }
}

static void build_inbox(lv_obj_t *page, const mirror_nav_t *nav, const notif_list_t *list) {
    if (list->count == 0) {
        lv_obj_t *t = text(page, &mirror_font_24, C_TEXT, "暂无消息");
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 110);
        lv_obj_t *h = text(page, &mirror_font_16, C_MUTED, "iPhone 上的新通知\n会出现在这里");
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 152);
        return;
    }
    for (size_t r = 0; r < INBOX_ROWS; r++) {
        lv_obj_t *row = box(page, 10, 36 + (int)r * INBOX_ROW_H, SCR_W - 20, INBOX_ROW_H - 4, C_PANEL);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_t *app = text(row, &mirror_font_16, C_BLUE, "");
        lv_obj_set_pos(app, 10, 3);
        lv_obj_set_width(app, 72);
        lv_label_set_long_mode(app, LV_LABEL_LONG_MODE_CLIP);
        lines(row, &mirror_font_16, C_TEXT, "", 86, 3, SCR_W - 20 - 96, 1);
        lines(row, &mirror_font_16, C_MUTED, "", 10, 25, SCR_W - 20 - 20, 1);
        s_w.rows[r] = row;
    }
    lv_obj_t *hint = text(page, &mirror_font_16, C_MUTED, "长按 OK 看全文");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 36 + INBOX_ROWS * INBOX_ROW_H + 2);
    refresh_inbox(nav, list);
}

// ---------------------------------------------------------------- 详情页

static void refresh_detail(const mirror_nav_t *nav) {
    uint8_t page = nav->detail_page < s_w.detail_pages ? nav->detail_page : s_w.detail_pages - 1;
    lv_obj_set_y(s_w.detail_body, -(int)page * DETAIL_STEP);
    lv_label_set_text_fmt(s_w.detail_more, s_w.detail_pages > 1 ? "▲▼ 翻页  %u/%u" : "OK 返回",
                          (unsigned)page + 1, (unsigned)s_w.detail_pages);
}

static void build_detail(lv_obj_t *page, const mirror_nav_t *nav, const notif_list_t *list,
                         uint32_t now_ms) {
    const notif_t *n = &list->items[nav->selected < list->count ? nav->selected : 0];
    chip(page, n->kind == NOTIF_KIND_CALL ? "来电" : n->app, kind_color(n->kind), 16, 36);
    char age[24];
    notif_fmt_age(now_ms - n->at_ms, age, sizeof(age));
    lv_obj_t *a = text(page, &mirror_font_16, C_MUTED, age);
    lv_obj_align(a, LV_ALIGN_TOP_RIGHT, -20, 38);
    lines(page, &mirror_font_24, C_TEXT, n->title, 16, 66, SCR_W - 32, 1);

    lv_obj_t *clip = lv_obj_create(page);
    lv_obj_remove_style_all(clip);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(clip, 16, DETAIL_TOP);
    lv_obj_set_size(clip, SCR_W - 32, DETAIL_H);
    s_w.detail_body = text(clip, &mirror_font_16, C_TEXT, n->body[0] ? n->body : "(没有正文)");
    lv_obj_set_width(s_w.detail_body, SCR_W - 32);
    lv_label_set_long_mode(s_w.detail_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_update_layout(s_w.detail_body);
    int h = lv_obj_get_height(s_w.detail_body);
    int pages = h <= DETAIL_H ? 1 : 1 + (h - DETAIL_H + DETAIL_STEP - 1) / DETAIL_STEP;
    s_w.detail_pages = (uint8_t)(pages > 255 ? 255 : pages);

    s_w.detail_more = text(page, &mirror_font_16, C_MUTED, "");
    lv_obj_align(s_w.detail_more, LV_ALIGN_TOP_MID, 0, DETAIL_TOP + DETAIL_H + 4);
    refresh_detail(nav);
}

uint8_t mirror_ui_detail_pages(void) {
    return s_w.detail_pages ? s_w.detail_pages : 1;
}

// ---------------------------------------------------------------- 连接页

static void refresh_status(const mirror_nav_t *nav) {
    if (!s_w.forget_hint) return;
    lv_label_set_text(s_w.forget_hint, nav->forget_armed ? "再长按 ▼ 一次即清除配对"
                                                         : "长按 ▼ 两次清除配对");
    lv_obj_set_style_text_color(s_w.forget_hint, lv_color_hex(nav->forget_armed ? C_RED : C_MUTED), 0);
}

static void build_status(lv_obj_t *page, const mirror_nav_t *nav, mirror_link_t link) {
    const char *head;
    const char *steps;
    uint32_t color;
    switch (link) {
    case MIRROR_LINK_READY:
        head = "已连接 iPhone";
        steps = "新通知会实时显示。\n导航时打开谷歌地图\n或苹果地图即可。";
        color = C_GREEN;
        break;
    case MIRROR_LINK_SECURING:
        head = "正在配对…";
        steps = "请在 iPhone 弹出的\n蓝牙配对请求中点\"配对\"。";
        color = C_AMBER;
        break;
    case MIRROR_LINK_DISCOVERING:
        head = "正在读取通知…";
        steps = "马上就好。";
        color = C_AMBER;
        break;
    case MIRROR_LINK_NO_ANCS:
        head = "缺少通知权限";
        steps = "iPhone 设置 → 蓝牙 →\n本设备右侧的 (i) →\n打开\"共享系统通知\",\n然后断开再连接一次。";
        color = C_RED;
        break;
    case MIRROR_LINK_OFF:
        head = "蓝牙启动失败";
        steps = "请关机后重新开机。";
        color = C_RED;
        break;
    case MIRROR_LINK_ADVERTISING:
    default:
        head = "等待 iPhone 连接";
        steps = "1. iPhone 设置 → 蓝牙\n2. 在\"其他设备\"里点\n    本设备的名字\n3. 点\"配对\",并允许\n    \"共享系统通知\"";
        color = C_TEXT;
        break;
    }
    lv_obj_t *dot = box(page, 24, 50, 12, 12, color);
    lv_obj_set_style_radius(dot, 6, 0);
    lv_obj_t *h = text(page, &mirror_font_24, color, head);
    lv_obj_set_pos(h, 44, 38);
    lv_obj_t *name = text(page, &mirror_font_16, C_MUTED, s_device_name);
    lv_obj_set_pos(name, 44, 72);
    box(page, 20, 102, SCR_W - 40, 1, C_LINE);
    lv_obj_t *s = text(page, &mirror_font_16, C_TEXT, steps);
    lv_obj_set_pos(s, 24, 114);
    lv_obj_set_style_text_line_space(s, 4, 0);
    s_w.forget_hint = text(page, &mirror_font_16, C_MUTED, "");
    lv_obj_align(s_w.forget_hint, LV_ALIGN_TOP_MID, 0, 290);
    refresh_status(nav);
}

// ---------------------------------------------------------------- 入口

void mirror_ui_show(const mirror_nav_t *nav, const notif_list_t *list, mirror_link_t link,
                    uint32_t now_ms) {
    if (s_page) lv_obj_delete(s_page);
    memset(&s_w, 0, sizeof(s_w));
    s_page = box(lv_screen_active(), 0, 0, SCR_W, SCR_H, C_BG);

    if (nav->detail && list->count) {
        build_detail(s_page, nav, list, now_ms);
    } else {
        switch (nav->page) {
        case MIRROR_PAGE_NAV:    build_nav(s_page, list, now_ms); break;
        case MIRROR_PAGE_INBOX:  build_inbox(s_page, nav, list); break;
        case MIRROR_PAGE_STATUS: build_status(s_page, nav, link); break;
        default: break;
        }
    }
    top_bar(nav->page, nav->detail, link);
}

void mirror_ui_refresh(const mirror_nav_t *nav, const notif_list_t *list, mirror_link_t link,
                       uint32_t now_ms) {
    top_bar(nav->page, nav->detail, link);
    if (nav->detail) {
        if (s_w.detail_body) refresh_detail(nav);
        return;
    }
    switch (nav->page) {
    case MIRROR_PAGE_INBOX:
        if (s_w.rows[0]) refresh_inbox(nav, list);
        break;
    case MIRROR_PAGE_STATUS:
        refresh_status(nav);
        break;
    case MIRROR_PAGE_NAV: {
        const notif_t *n = notif_list_latest_nav(list);
        if (s_w.nav_age && n) {
            char age[24];
            notif_fmt_age(now_ms - n->at_ms, age, sizeof(age));
            lv_label_set_text_fmt(s_w.nav_age, "%s更新", age);
        }
        break;
    }
    default:
        break;
    }
}
