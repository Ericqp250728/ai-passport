// main/badge_ui.c —— 摊主胸牌界面:招牌红 + 米白底,面向顾客的大字号,面向摊主的记账页。
//
// 屏幕是 240x320 竖屏,四角被 BSP 以 30px 圆角遮黑,所以所有内容都离角落留出余量。
// 顶部状态栏(页码点 + 电量)和提示浮层放在 lv_layer_top(),切页时不重建。
// 本文件的所有中文都会被 tools/gen_badge_fonts.py 收进字库;改了文案要重新生成字库。
#include "badge_ui.h"

#include "badge_fmt.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(badge_font_16)
LV_FONT_DECLARE(badge_font_20)
LV_FONT_DECLARE(badge_font_32)
LV_FONT_DECLARE(badge_num_48)

#define SCR_W 240
#define SCR_H 320

#define COLOR_RED    0xD9412B   // 招牌红
#define COLOR_CREAM  0xFFF6E6   // 米白底
#define COLOR_INK    0x2B2118   // 墨色正文
#define COLOR_MUTED  0x8A7B6A   // 次要文字
#define COLOR_LINE   0xE9DCC6   // 分隔线
#define COLOR_GOLD   0xFFE3B0   // 红底上的浅金字
#define COLOR_GREEN  0x2E8B57   // 成功提示

#define BANNER_H        150
#define LEDGER_ROWS     4
#define QR_SIZE         196

static const badge_content_t *s_content;
static size_t s_items;                   // 内容文件里的数量,已截断到数组容量
static size_t s_pays;
static size_t s_lines;
static lv_obj_t *s_page;                 // 当前页容器,切页时整个删除重建
static lv_obj_t *s_dots[BADGE_PAGE_COUNT];
static lv_obj_t *s_battery;
static lv_obj_t *s_toast;
static lv_obj_t *s_toast_label;
static lv_timer_t *s_toast_timer;
static int s_soc = -1;
static bool s_top_on_red;
static badge_page_t s_cur_page;

// 各页需要局部刷新的对象;切页时清空,避免指向已删除对象。
static struct {
    lv_obj_t *qr;
    lv_obj_t *qr_label;
    lv_obj_t *qr_hint;
    lv_obj_t *qr_placeholder;
    lv_obj_t *rows[BADGE_MAX_ITEMS];
    lv_obj_t *row_names[BADGE_MAX_ITEMS];
    lv_obj_t *row_prices[BADGE_MAX_ITEMS];
    lv_obj_t *sell_total;
    lv_obj_t *ledger_total;
    lv_obj_t *ledger_count;
    lv_obj_t *ledger_rows[LEDGER_ROWS];
    lv_obj_t *ledger_hint;
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

// 固定宽高的单行:超长时末尾显示省略号,而不是折行挤占下面的内容。
static void one_line(lv_obj_t *l, const lv_font_t *font, int width) {
    lv_obj_set_size(l, width, lv_font_get_line_height(font));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
}

// 固定宽度、居中、超长显示省略号的一行字。
static lv_obj_t *centered(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                          const char *str, int y, int width) {
    lv_obj_t *l = text(parent, font, color, str);
    one_line(l, font, width);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

// ---------------------------------------------------------------- 顶部状态栏

static void top_bar_refresh(badge_page_t page) {
    uint32_t on = s_top_on_red ? 0xFFFFFF : COLOR_RED;
    uint32_t off = s_top_on_red ? 0xF29A88 : COLOR_LINE;
    s_cur_page = page;
    int x = 36;
    for (int i = 0; i < BADGE_PAGE_COUNT; i++) {
        bool cur = (badge_page_t)i == page;
        int w = cur ? 16 : 6;
        lv_obj_set_pos(s_dots[i], x, 14);
        lv_obj_set_width(s_dots[i], w);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(cur ? on : off), 0);
        x += w + 5;
    }

    if (s_soc < 0) {
        lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_battery, "%d%%", s_soc);
    uint32_t color = s_soc <= 15 ? (s_top_on_red ? COLOR_GOLD : COLOR_RED)
                                 : (s_top_on_red ? 0xFFFFFF : COLOR_MUTED);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(color), 0);
}

static void toast_hide_cb(lv_timer_t *t) {
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(t);
}

void badge_ui_init(const badge_content_t *content) {
    s_content = content;
    s_items = content->item_count < BADGE_MAX_ITEMS ? content->item_count : BADGE_MAX_ITEMS;
    s_pays = content->pay_count < BADGE_MAX_PAY ? content->pay_count : BADGE_MAX_PAY;
    s_lines = content->line_count < BADGE_MAX_LINES ? content->line_count : BADGE_MAX_LINES;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_CREAM), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *top = lv_layer_top();
    for (int i = 0; i < BADGE_PAGE_COUNT; i++) {
        s_dots[i] = box(top, 36, 14, 6, 6, COLOR_LINE);
        lv_obj_set_style_radius(s_dots[i], 3, 0);
    }
    s_battery = text(top, &badge_font_16, COLOR_MUTED, "");
    lv_obj_set_width(s_battery, 52);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_battery, 152, 5);

    s_toast = box(top, 20, 264, 200, 44, COLOR_INK);
    lv_obj_set_style_radius(s_toast, 22, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    s_toast_label = text(s_toast, &badge_font_20, 0xFFFFFF, "");
    one_line(s_toast_label, &badge_font_20, 184);
    lv_obj_set_style_text_align(s_toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_toast_label);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_timer = lv_timer_create(toast_hide_cb, 1500, NULL);
    lv_timer_pause(s_toast_timer);
}

void badge_ui_set_battery(int soc) {
    s_soc = soc > 100 ? 100 : soc;
    top_bar_refresh(s_cur_page);
}

void badge_ui_toast(const char *str, bool error) {
    lv_label_set_text(s_toast_label, str);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(error ? COLOR_RED : COLOR_INK), 0);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    lv_timer_reset(s_toast_timer);
    lv_timer_resume(s_toast_timer);
}

// ---------------------------------------------------------------- 招牌页

static void build_sign(lv_obj_t *page) {
    box(page, 0, 0, SCR_W, BANNER_H, COLOR_RED);
    centered(page, &badge_font_32, 0xFFFFFF, s_content->shop, 44, 208);
    centered(page, &badge_font_16, COLOR_GOLD, s_content->slogan, 98, 200);
    // 招牌底边的一道浅金细线,像木招牌的描边
    box(page, 40, 130, 160, 2, COLOR_GOLD);

    for (size_t i = 0; i < s_lines; i++) {
        int y = 168 + (int)i * 36;
        lv_obj_t *dot = box(page, 30, y + 11, 7, 7, COLOR_RED);
        lv_obj_set_style_radius(dot, 2, 0);
        lv_obj_t *l = text(page, &badge_font_20, COLOR_INK, s_content->lines[i]);
        one_line(l, &badge_font_20, 174);
        lv_obj_set_pos(l, 46, y);
    }
}

// ---------------------------------------------------------------- 收款页

static void refresh_pay(const badge_nav_t *nav) {
    if (s_pays == 0) return;
    const badge_pay_t *pay = &s_content->pay[nav->pay_index];
    lv_qrcode_update(s_w.qr, pay->payload, (uint32_t)strlen(pay->payload));
    lv_label_set_text(s_w.qr_label, pay->label);
    if (s_pays > 1) {
        lv_label_set_text_fmt(s_w.qr_hint, "▲▼ 切换  %u/%u",
                              (unsigned)nav->pay_index + 1, (unsigned)s_pays);
    }
    if (badge_pay_is_placeholder(pay->payload)) {
        lv_obj_remove_flag(s_w.qr_placeholder, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_w.qr_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_pay(lv_obj_t *page, const badge_nav_t *nav) {
    lv_obj_set_style_bg_color(page, lv_color_white(), 0);
    centered(page, &badge_font_20, COLOR_INK, "扫码付款", 32, 200);

    if (s_pays == 0) {
        centered(page, &badge_font_20, COLOR_MUTED, "还没有配置收款码", 150, 200);
        return;
    }
    s_w.qr = lv_qrcode_create(page);
    lv_qrcode_set_size(s_w.qr, QR_SIZE);
    lv_qrcode_set_dark_color(s_w.qr, lv_color_black());
    lv_qrcode_set_light_color(s_w.qr, lv_color_white());
    lv_obj_set_pos(s_w.qr, (SCR_W - QR_SIZE) / 2, 64);

    // 占位码盖一条红带:避免顾客真的去扫一个示例链接
    s_w.qr_placeholder = box(page, 0, 64 + QR_SIZE / 2 - 22, SCR_W, 44, COLOR_RED);
    lv_obj_t *warn = text(s_w.qr_placeholder, &badge_font_16, 0xFFFFFF, "示例码 · 请换成你的收款码");
    lv_obj_center(warn);

    s_w.qr_label = centered(page, &badge_font_20, COLOR_INK, "", 266, 200);
    s_w.qr_hint = centered(page, &badge_font_16, COLOR_MUTED, "", 294, 160);
    refresh_pay(nav);
}

// ---------------------------------------------------------------- 记账页

static void refresh_sell(const badge_nav_t *nav, const badge_ledger_t *ledger) {
    for (size_t i = 0; i < s_items; i++) {
        bool sel = i == nav->sell_index;
        lv_obj_set_style_bg_opa(s_w.rows[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        uint32_t fg = sel ? 0xFFFFFF : COLOR_INK;
        lv_obj_set_style_text_color(s_w.row_names[i], lv_color_hex(fg), 0);
        lv_obj_set_style_text_color(s_w.row_prices[i], lv_color_hex(sel ? 0xFFFFFF : COLOR_RED), 0);
    }
    char total[24];
    badge_fmt_cents(ledger->total_cents, true, total, sizeof(total));
    lv_label_set_text_fmt(s_w.sell_total, "本场 %s · %u 笔", total, (unsigned)ledger->sales);
}

static void build_sell(lv_obj_t *page, const badge_nav_t *nav, const badge_ledger_t *ledger) {
    lv_obj_t *title = text(page, &badge_font_20, COLOR_INK, "记一笔");
    lv_obj_set_pos(title, 26, 30);
    lv_obj_t *hint = text(page, &badge_font_16, COLOR_MUTED, "长按 OK 记账");
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -24, 34);

    if (s_items == 0) {
        centered(page, &badge_font_20, COLOR_MUTED, "还没有配置商品", 150, 200);
        s_w.sell_total = centered(page, &badge_font_16, COLOR_MUTED, "", 280, 200);
        refresh_sell(nav, ledger);
        return;
    }

    int row_h = 206 / (int)s_items;
    if (row_h > 36) row_h = 36;
    int top = 66 + (206 - row_h * (int)s_items) / 2;
    for (size_t i = 0; i < s_items; i++) {
        lv_obj_t *row = box(page, 16, top + (int)i * row_h, SCR_W - 32, row_h - 2, COLOR_RED);
        lv_obj_set_style_radius(row, 8, 0);
        s_w.rows[i] = row;

        lv_obj_t *name = text(row, &badge_font_20, COLOR_INK, s_content->items[i].name);
        one_line(name, &badge_font_20, 120);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
        s_w.row_names[i] = name;

        char price[24];
        badge_fmt_cents(s_content->items[i].price_cents, true, price, sizeof(price));
        lv_obj_t *p = text(row, &badge_font_20, COLOR_RED, price);
        lv_obj_align(p, LV_ALIGN_RIGHT_MID, -12, 0);
        s_w.row_prices[i] = p;
    }

    box(page, 24, 274, SCR_W - 48, 1, COLOR_LINE);
    s_w.sell_total = centered(page, &badge_font_16, COLOR_MUTED, "", 282, 200);
    refresh_sell(nav, ledger);
}

// ---------------------------------------------------------------- 账本页

static void refresh_ledger(const badge_nav_t *nav, const badge_ledger_t *ledger) {
    char buf[32];
    badge_fmt_cents(ledger->total_cents, true, buf, sizeof(buf));
    // 48px 数字最多放得下约 8 个字符,更长的金额退回 32px 粗体
    lv_obj_set_style_text_font(s_w.ledger_total, strlen(buf) > 9 ? &badge_font_32 : &badge_num_48, 0);
    lv_label_set_text(s_w.ledger_total, buf);
    lv_label_set_text_fmt(s_w.ledger_count, "共 %u 笔", (unsigned)ledger->sales);

    // 按收入从高到低列出卖出过的商品,最多 LEDGER_ROWS 行
    size_t order[BADGE_MAX_ITEMS];
    size_t sold = 0;
    for (size_t i = 0; i < s_items; i++) {
        if (ledger->item_count[i] == 0) continue;
        size_t k = sold++;
        while (k > 0 && ledger->item_cents[order[k - 1]] < ledger->item_cents[i]) {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = i;
    }
    // 卖出种类多于行数时,最后一行合并为"其他 n 种"。
    size_t shown = sold > LEDGER_ROWS ? LEDGER_ROWS - 1 : sold;
    for (size_t r = 0; r < LEDGER_ROWS; r++) {
        lv_obj_t *row = s_w.ledger_rows[r];
        lv_obj_t *name = lv_obj_get_child(row, 0);
        lv_obj_t *amount = lv_obj_get_child(row, 1);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        if (r < shown) {
            size_t i = order[r];
            lv_label_set_text_fmt(name, "%s × %u",
                                  s_content->items[i].name, (unsigned)ledger->item_count[i]);
            badge_fmt_cents(ledger->item_cents[i], true, buf, sizeof(buf));
            lv_label_set_text(amount, buf);
        } else if (r == shown && sold > shown) {
            uint32_t rest = 0;
            for (size_t k = shown; k < sold; k++) rest += ledger->item_cents[order[k]];
            lv_label_set_text_fmt(name, "其他 %u 种", (unsigned)(sold - shown));
            badge_fmt_cents(rest, true, buf, sizeof(buf));
            lv_label_set_text(amount, buf);
        } else if (r == 0 && sold == 0) {
            lv_label_set_text(name, "还没有记账");
            lv_label_set_text(amount, "");
        } else {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (nav->clear_armed) {
        lv_label_set_text(s_w.ledger_hint, "再长按 ▼ 一次即清零");
        lv_obj_set_style_text_color(s_w.ledger_hint, lv_color_hex(COLOR_RED), 0);
    } else {
        lv_label_set_text(s_w.ledger_hint, "长按 ▼ 两次清零");
        lv_obj_set_style_text_color(s_w.ledger_hint, lv_color_hex(COLOR_MUTED), 0);
    }
}

static void build_ledger(lv_obj_t *page, const badge_nav_t *nav, const badge_ledger_t *ledger) {
    lv_obj_t *title = text(page, &badge_font_20, COLOR_INK, "本场账本");
    lv_obj_set_pos(title, 26, 30);

    s_w.ledger_total = text(page, &badge_num_48, COLOR_RED, "");
    lv_obj_set_width(s_w.ledger_total, 216);
    lv_obj_set_style_text_align(s_w.ledger_total, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_w.ledger_total, LV_ALIGN_TOP_MID, 0, 66);
    s_w.ledger_count = centered(page, &badge_font_16, COLOR_MUTED, "", 134, 200);
    box(page, 24, 162, SCR_W - 48, 1, COLOR_LINE);

    for (size_t r = 0; r < LEDGER_ROWS; r++) {
        lv_obj_t *row = lv_obj_create(page);
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(row, 28, 166 + (int)r * 24);
        lv_obj_set_size(row, SCR_W - 56, 24);
        lv_obj_t *name = text(row, &badge_font_16, COLOR_INK, "");
        one_line(name, &badge_font_16, 120);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *amount = text(row, &badge_font_16, COLOR_INK, "");
        lv_obj_align(amount, LV_ALIGN_RIGHT_MID, 0, 0);
        s_w.ledger_rows[r] = row;
    }
    centered(page, &badge_font_16, COLOR_MUTED, "长按 OK 撤销上一笔", 266, 200);
    s_w.ledger_hint = centered(page, &badge_font_16, COLOR_MUTED, "", 290, 170);
    refresh_ledger(nav, ledger);
}

// ---------------------------------------------------------------- 入口

void badge_ui_show_page(const badge_nav_t *nav, const badge_ledger_t *ledger) {
    if (s_page) lv_obj_delete(s_page);
    memset(&s_w, 0, sizeof(s_w));

    s_page = box(lv_screen_active(), 0, 0, SCR_W, SCR_H, COLOR_CREAM);
    s_top_on_red = nav->page == BADGE_PAGE_SIGN;
    switch (nav->page) {
    case BADGE_PAGE_SIGN:   build_sign(s_page); break;
    case BADGE_PAGE_PAY:    build_pay(s_page, nav); break;
    case BADGE_PAGE_SELL:   build_sell(s_page, nav, ledger); break;
    case BADGE_PAGE_LEDGER: build_ledger(s_page, nav, ledger); break;
    default: break;
    }
    top_bar_refresh(nav->page);
}

void badge_ui_refresh(const badge_nav_t *nav, const badge_ledger_t *ledger) {
    switch (nav->page) {
    case BADGE_PAGE_PAY:    if (s_w.qr) refresh_pay(nav); break;
    case BADGE_PAGE_SELL:   if (s_w.sell_total) refresh_sell(nav, ledger); break;
    case BADGE_PAGE_LEDGER: if (s_w.ledger_total) refresh_ledger(nav, ledger); break;
    default: break;
    }
}
