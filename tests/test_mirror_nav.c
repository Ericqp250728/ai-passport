#include <assert.h>

#include "mirror_nav.h"

static mirror_act_t click(mirror_nav_t *n, mirror_key_t k, uint32_t t) {
    return mirror_nav_input(n, k, MIRROR_PRESS_CLICK, t);
}

static mirror_act_t hold(mirror_nav_t *n, mirror_key_t k, uint32_t t) {
    return mirror_nav_input(n, k, MIRROR_PRESS_LONG, t);
}

static void test_pages_and_inbox(void) {
    mirror_nav_t n;
    mirror_nav_init(&n, 0);
    assert(n.page == MIRROR_PAGE_NAV);
    assert(click(&n, MIRROR_KEY_OK, 10) == MIRROR_ACT_PAGE && n.page == MIRROR_PAGE_INBOX);
    assert(hold(&n, MIRROR_KEY_OK, 20) == MIRROR_ACT_NONE);           // 没有消息:不开详情

    mirror_nav_set_count(&n, 3, true);
    assert(click(&n, MIRROR_KEY_UP, 30) == MIRROR_ACT_SELECT && n.selected == 2);
    assert(click(&n, MIRROR_KEY_DOWN, 40) == MIRROR_ACT_SELECT && n.selected == 0);
    assert(click(&n, MIRROR_KEY_DOWN, 50) == MIRROR_ACT_SELECT && n.selected == 1);

    assert(hold(&n, MIRROR_KEY_OK, 60) == MIRROR_ACT_PAGE && n.detail);
    assert(click(&n, MIRROR_KEY_UP, 70) == MIRROR_ACT_NONE);          // 已在第一页
    assert(click(&n, MIRROR_KEY_DOWN, 80) == MIRROR_ACT_SELECT && n.detail_page == 1);
    assert(click(&n, MIRROR_KEY_UP, 90) == MIRROR_ACT_SELECT && n.detail_page == 0);
    assert(click(&n, MIRROR_KEY_OK, 100) == MIRROR_ACT_PAGE);
    assert(!n.detail && n.page == MIRROR_PAGE_INBOX && n.selected == 1);

    assert(click(&n, MIRROR_KEY_OK, 110) == MIRROR_ACT_PAGE && n.page == MIRROR_PAGE_STATUS);
    assert(click(&n, MIRROR_KEY_OK, 120) == MIRROR_ACT_PAGE && n.page == MIRROR_PAGE_NAV);
}

static void test_count_changes(void) {
    mirror_nav_t n;
    mirror_nav_init(&n, 0);
    n.page = MIRROR_PAGE_INBOX;
    mirror_nav_set_count(&n, 4, true);
    n.selected = 3;
    assert(mirror_nav_set_count(&n, 2, true) == MIRROR_ACT_SELECT && n.selected == 1);

    assert(hold(&n, MIRROR_KEY_OK, 10) == MIRROR_ACT_PAGE && n.detail);
    // 正在看的消息被删除:关闭详情
    assert(mirror_nav_set_count(&n, 1, false) == MIRROR_ACT_PAGE && !n.detail);
    assert(mirror_nav_set_count(&n, 0, true) == MIRROR_ACT_SELECT && n.selected == 0);

    n.page = MIRROR_PAGE_NAV;
    assert(mirror_nav_set_count(&n, 3, true) == MIRROR_ACT_NONE);
}

static void test_notifications_switch_to_nav(void) {
    mirror_nav_t n;
    mirror_nav_init(&n, 0);
    n.page = MIRROR_PAGE_STATUS;
    assert(mirror_nav_on_notif(&n, true, 100) == MIRROR_ACT_PAGE && n.page == MIRROR_PAGE_NAV);
    n.page = MIRROR_PAGE_INBOX;
    assert(mirror_nav_on_notif(&n, false, 200) == MIRROR_ACT_BACKLIGHT && n.page == MIRROR_PAGE_INBOX);

    // 正在看详情时,导航更新不打断
    mirror_nav_set_count(&n, 1, true);
    assert(hold(&n, MIRROR_KEY_OK, 300) == MIRROR_ACT_PAGE && n.detail);
    assert(mirror_nav_on_notif(&n, true, 400) == MIRROR_ACT_BACKLIGHT && n.detail);
}

static void test_backlight(void) {
    mirror_nav_t n;
    mirror_nav_init(&n, 0);
    assert(mirror_nav_backlight(&n, 0) == MIRROR_BRIGHT_PERCENT);
    assert(mirror_nav_tick(&n, MIRROR_AWAKE_MS - 1) == MIRROR_ACT_NONE);
    assert(mirror_nav_tick(&n, MIRROR_AWAKE_MS) == MIRROR_ACT_BACKLIGHT);
    assert(n.backlight == MIRROR_DIM_PERCENT);
    assert(mirror_nav_tick(&n, MIRROR_DIM_MS) == MIRROR_ACT_BACKLIGHT && n.backlight == 0);
    assert(mirror_nav_tick(&n, MIRROR_DIM_MS + 500) == MIRROR_ACT_NONE);

    // 熄屏时第一下只点亮,不翻页
    assert(click(&n, MIRROR_KEY_OK, MIRROR_DIM_MS + 1000) == MIRROR_ACT_WAKE);
    assert(n.page == MIRROR_PAGE_NAV && mirror_nav_backlight(&n, MIRROR_DIM_MS + 1000) == 80);
    assert(click(&n, MIRROR_KEY_OK, MIRROR_DIM_MS + 1100) == MIRROR_ACT_PAGE);

    // 通知点亮 15 秒,之后按空闲时长回落
    uint32_t t = 500000;
    assert(mirror_nav_tick(&n, t) == MIRROR_ACT_BACKLIGHT && n.backlight == 0);
    mirror_nav_on_notif(&n, false, t);
    assert(mirror_nav_backlight(&n, t + MIRROR_NOTIF_WAKE_MS - 1) == MIRROR_BRIGHT_PERCENT);
    assert(mirror_nav_tick(&n, t + MIRROR_NOTIF_WAKE_MS) == MIRROR_ACT_BACKLIGHT && n.backlight == 0);
}

static void test_forget_needs_two_long_presses(void) {
    mirror_nav_t n;
    mirror_nav_init(&n, 0);
    n.page = MIRROR_PAGE_STATUS;
    assert(hold(&n, MIRROR_KEY_DOWN, 100) == MIRROR_ACT_FORGET_ARM);
    assert(hold(&n, MIRROR_KEY_DOWN, 200) == MIRROR_ACT_FORGET);

    assert(hold(&n, MIRROR_KEY_DOWN, 300) == MIRROR_ACT_FORGET_ARM);
    assert(click(&n, MIRROR_KEY_UP, 400) == MIRROR_ACT_FORGET_DISARM && !n.forget_armed);

    assert(hold(&n, MIRROR_KEY_DOWN, 500) == MIRROR_ACT_FORGET_ARM);
    assert(mirror_nav_tick(&n, 500 + MIRROR_CONFIRM_MS) == MIRROR_ACT_FORGET_DISARM);
    // 过期后的长按重新进入待确认
    assert(hold(&n, MIRROR_KEY_DOWN, 4000) == MIRROR_ACT_FORGET_ARM);
    assert(hold(&n, MIRROR_KEY_DOWN, 4000 + MIRROR_CONFIRM_MS) == MIRROR_ACT_FORGET_ARM);

    n.page = MIRROR_PAGE_NAV;
    n.forget_armed = false;
    assert(hold(&n, MIRROR_KEY_DOWN, 9000) == MIRROR_ACT_NONE);
}

static void test_wraparound(void) {
    mirror_nav_t n;
    uint32_t t0 = UINT32_MAX - 5000;
    mirror_nav_init(&n, t0);
    assert(mirror_nav_backlight(&n, t0 + 4000) == MIRROR_BRIGHT_PERCENT);
    assert(mirror_nav_backlight(&n, t0 + MIRROR_AWAKE_MS) == MIRROR_DIM_PERCENT);
    n.page = MIRROR_PAGE_STATUS;
    n.last_input_ms = UINT32_MAX - 100;
    assert(hold(&n, MIRROR_KEY_DOWN, UINT32_MAX - 50) == MIRROR_ACT_FORGET_ARM);
    assert(hold(&n, MIRROR_KEY_DOWN, 100) == MIRROR_ACT_FORGET);
}

int main(void) {
    test_pages_and_inbox();
    test_count_changes();
    test_notifications_switch_to_nav();
    test_backlight();
    test_forget_needs_two_long_presses();
    test_wraparound();
    return 0;
}
