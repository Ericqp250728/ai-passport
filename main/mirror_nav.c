// main/mirror_nav.c —— 页面与亮屏状态机。时间为 uint32 毫秒,差值运算可跨越回绕。
#include "mirror_nav.h"

static bool before(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) < 0;
}

uint8_t mirror_nav_backlight(const mirror_nav_t *nav, uint32_t now_ms) {
    if (before(now_ms, nav->wake_until_ms)) return MIRROR_BRIGHT_PERCENT;
    uint32_t idle = now_ms - nav->last_input_ms;
    if (idle < MIRROR_AWAKE_MS) return MIRROR_BRIGHT_PERCENT;
    if (idle < MIRROR_DIM_MS) return MIRROR_DIM_PERCENT;
    return 0;
}

void mirror_nav_init(mirror_nav_t *nav, uint32_t now_ms) {
    *nav = (mirror_nav_t){
        .page = MIRROR_PAGE_NAV,
        .last_input_ms = now_ms,
        .wake_until_ms = now_ms,
        .backlight = MIRROR_BRIGHT_PERCENT,
    };
}

static mirror_act_t go(mirror_nav_t *nav, mirror_page_t page) {
    nav->page = page;
    nav->detail = false;
    nav->detail_page = 0;
    nav->forget_armed = false;
    return MIRROR_ACT_PAGE;
}

static mirror_act_t on_click(mirror_nav_t *nav, mirror_key_t key) {
    if (nav->detail) {
        if (key == MIRROR_KEY_OK) return go(nav, MIRROR_PAGE_INBOX);
        if (key == MIRROR_KEY_DOWN) {
            nav->detail_page++;
            return MIRROR_ACT_SELECT;
        }
        if (nav->detail_page == 0) return MIRROR_ACT_NONE;
        nav->detail_page--;
        return MIRROR_ACT_SELECT;
    }
    if (key == MIRROR_KEY_OK) return go(nav, (mirror_page_t)((nav->page + 1) % MIRROR_PAGE_COUNT));
    if (nav->page != MIRROR_PAGE_INBOX || nav->count < 2) return MIRROR_ACT_NONE;
    if (key == MIRROR_KEY_DOWN) nav->selected = (nav->selected + 1) % nav->count;
    else nav->selected = (nav->selected + nav->count - 1) % nav->count;
    return MIRROR_ACT_SELECT;
}

static mirror_act_t on_long(mirror_nav_t *nav, mirror_key_t key, uint32_t now_ms, bool confirms) {
    if (key == MIRROR_KEY_OK && nav->page == MIRROR_PAGE_INBOX && !nav->detail && nav->count > 0) {
        nav->detail = true;
        nav->detail_page = 0;
        return MIRROR_ACT_PAGE;
    }
    if (key == MIRROR_KEY_DOWN && nav->page == MIRROR_PAGE_STATUS) {
        if (confirms) {
            nav->forget_armed = false;
            return MIRROR_ACT_FORGET;
        }
        nav->forget_armed = true;
        nav->forget_deadline_ms = now_ms + MIRROR_CONFIRM_MS;
        return MIRROR_ACT_FORGET_ARM;
    }
    return MIRROR_ACT_NONE;
}

mirror_act_t mirror_nav_input(mirror_nav_t *nav, mirror_key_t key, mirror_press_t press,
                              uint32_t now_ms) {
    bool was_dark = mirror_nav_backlight(nav, now_ms) < MIRROR_BRIGHT_PERCENT;
    nav->last_input_ms = now_ms;
    nav->backlight = MIRROR_BRIGHT_PERCENT;
    if (was_dark) return MIRROR_ACT_WAKE;

    bool was_armed = nav->forget_armed;
    bool confirms = was_armed && press == MIRROR_PRESS_LONG && key == MIRROR_KEY_DOWN
                 && before(now_ms, nav->forget_deadline_ms);
    if (was_armed && !confirms) nav->forget_armed = false;

    mirror_act_t act = press == MIRROR_PRESS_LONG ? on_long(nav, key, now_ms, confirms)
                                                  : on_click(nav, key);
    if (was_armed && !confirms && act == MIRROR_ACT_NONE) return MIRROR_ACT_FORGET_DISARM;
    return act;
}

mirror_act_t mirror_nav_set_count(mirror_nav_t *nav, size_t count, bool selected_still_exists) {
    nav->count = count;
    if (nav->selected >= count) nav->selected = count ? count - 1 : 0;
    if (nav->detail && (!selected_still_exists || count == 0)) {
        nav->detail = false;
        nav->detail_page = 0;
        return MIRROR_ACT_PAGE;
    }
    return nav->page == MIRROR_PAGE_INBOX && !nav->detail ? MIRROR_ACT_SELECT : MIRROR_ACT_NONE;
}

mirror_act_t mirror_nav_on_notif(mirror_nav_t *nav, bool is_nav, uint32_t now_ms) {
    nav->wake_until_ms = now_ms + MIRROR_NOTIF_WAKE_MS;
    nav->backlight = MIRROR_BRIGHT_PERCENT;
    if (is_nav && !nav->detail && nav->page != MIRROR_PAGE_NAV) return go(nav, MIRROR_PAGE_NAV);
    return MIRROR_ACT_BACKLIGHT;
}

mirror_act_t mirror_nav_tick(mirror_nav_t *nav, uint32_t now_ms) {
    if (nav->forget_armed && !before(now_ms, nav->forget_deadline_ms)) {
        nav->forget_armed = false;
        return MIRROR_ACT_FORGET_DISARM;
    }
    uint8_t level = mirror_nav_backlight(nav, now_ms);
    if (level != nav->backlight) {
        nav->backlight = level;
        return MIRROR_ACT_BACKLIGHT;
    }
    return MIRROR_ACT_NONE;
}
