#include <assert.h>

#include "badge_nav.h"

static badge_action_t click(badge_nav_t *n, badge_key_t k, uint32_t t) {
    return badge_nav_input(n, k, BADGE_PRESS_CLICK, t);
}

static badge_action_t hold(badge_nav_t *n, badge_key_t k, uint32_t t) {
    return badge_nav_input(n, k, BADGE_PRESS_LONG, t);
}

static void test_page_cycle_and_shortcuts(void) {
    badge_nav_t n;
    badge_nav_init(&n, 2, 5, 0);
    assert(n.page == BADGE_PAGE_SIGN);
    assert(click(&n, BADGE_KEY_OK, 10) == BADGE_ACTION_PAGE && n.page == BADGE_PAGE_PAY);
    assert(click(&n, BADGE_KEY_OK, 20) == BADGE_ACTION_PAGE && n.page == BADGE_PAGE_SELL);
    assert(click(&n, BADGE_KEY_OK, 30) == BADGE_ACTION_PAGE && n.page == BADGE_PAGE_LEDGER);
    assert(click(&n, BADGE_KEY_OK, 40) == BADGE_ACTION_PAGE && n.page == BADGE_PAGE_SIGN);

    // 招牌页长按 OK 直达收款码,收款页长按 OK 回招牌
    assert(hold(&n, BADGE_KEY_OK, 50) == BADGE_ACTION_PAGE && n.page == BADGE_PAGE_PAY);
    assert(hold(&n, BADGE_KEY_OK, 60) == BADGE_ACTION_PAGE && n.page == BADGE_PAGE_SIGN);
}

static void test_in_page_selection(void) {
    badge_nav_t n;
    badge_nav_init(&n, 2, 3, 0);
    n.page = BADGE_PAGE_PAY;
    assert(click(&n, BADGE_KEY_DOWN, 1) == BADGE_ACTION_SELECT && n.pay_index == 1);
    assert(click(&n, BADGE_KEY_DOWN, 2) == BADGE_ACTION_SELECT && n.pay_index == 0);
    assert(click(&n, BADGE_KEY_UP, 3) == BADGE_ACTION_SELECT && n.pay_index == 1);

    n.page = BADGE_PAGE_SELL;
    assert(click(&n, BADGE_KEY_UP, 4) == BADGE_ACTION_SELECT && n.sell_index == 2);
    assert(click(&n, BADGE_KEY_DOWN, 5) == BADGE_ACTION_SELECT && n.sell_index == 0);
    assert(hold(&n, BADGE_KEY_OK, 6) == BADGE_ACTION_RECORD);

    // 只有一个收款码时上下键无动作
    badge_nav_init(&n, 1, 0, 0);
    n.page = BADGE_PAGE_PAY;
    assert(click(&n, BADGE_KEY_DOWN, 1) == BADGE_ACTION_NONE);
    // 没有商品时长按不记账
    n.page = BADGE_PAGE_SELL;
    assert(hold(&n, BADGE_KEY_OK, 2) == BADGE_ACTION_NONE);
}

static void test_brightness(void) {
    badge_nav_t n;
    badge_nav_init(&n, 1, 1, 0);
    assert(badge_nav_backlight(&n) == 100);
    assert(click(&n, BADGE_KEY_UP, 1) == BADGE_ACTION_NONE);          // 已是最亮
    assert(click(&n, BADGE_KEY_DOWN, 2) == BADGE_ACTION_BRIGHTNESS);
    assert(badge_nav_backlight(&n) == 70);
    assert(click(&n, BADGE_KEY_DOWN, 3) == BADGE_ACTION_BRIGHTNESS);
    assert(click(&n, BADGE_KEY_DOWN, 4) == BADGE_ACTION_NONE);        // 已是最暗档
    assert(badge_nav_backlight(&n) == 40);
    // 收款页始终全亮,方便扫码
    n.page = BADGE_PAGE_PAY;
    assert(badge_nav_backlight(&n) == 100);
}

static void test_clear_needs_two_long_presses(void) {
    badge_nav_t n;
    badge_nav_init(&n, 1, 1, 0);
    n.page = BADGE_PAGE_LEDGER;
    assert(hold(&n, BADGE_KEY_DOWN, 1000) == BADGE_ACTION_CLEAR_ARM && n.clear_armed);
    assert(hold(&n, BADGE_KEY_DOWN, 2000) == BADGE_ACTION_CLEAR && !n.clear_armed);

    // 中间按了别的键:取消待确认
    assert(hold(&n, BADGE_KEY_DOWN, 3000) == BADGE_ACTION_CLEAR_ARM);
    assert(click(&n, BADGE_KEY_UP, 3100) == BADGE_ACTION_CLEAR_DISARM && !n.clear_armed);
    assert(hold(&n, BADGE_KEY_DOWN, 3200) == BADGE_ACTION_CLEAR_ARM);

    // 长按 OK(撤销)也会取消待确认,但撤销照常执行
    assert(hold(&n, BADGE_KEY_OK, 3300) == BADGE_ACTION_UNDO && !n.clear_armed);

    // 超时后由 tick 收回提示;之后的长按↓重新进入待确认,而不是清零
    assert(hold(&n, BADGE_KEY_DOWN, 4000) == BADGE_ACTION_CLEAR_ARM);
    assert(badge_nav_tick(&n, 4000 + BADGE_CLEAR_CONFIRM_MS - 1) == BADGE_ACTION_NONE);
    assert(badge_nav_tick(&n, 4000 + BADGE_CLEAR_CONFIRM_MS) == BADGE_ACTION_CLEAR_DISARM);
    assert(hold(&n, BADGE_KEY_DOWN, 8000) == BADGE_ACTION_CLEAR_ARM);

    // tick 还没来得及收回时,过期的第二次长按也不能清零
    assert(hold(&n, BADGE_KEY_DOWN, 8000 + BADGE_CLEAR_CONFIRM_MS + 1) == BADGE_ACTION_CLEAR_ARM);

    // 其他页面的长按↓不会触发清零
    n.page = BADGE_PAGE_SELL;
    n.clear_armed = false;
    assert(hold(&n, BADGE_KEY_DOWN, 20000) == BADGE_ACTION_NONE);
}

static void test_idle_timeouts(void) {
    badge_nav_t n;
    badge_nav_init(&n, 1, 1, 0);
    n.page = BADGE_PAGE_LEDGER;
    n.last_input_ms = 100;
    assert(badge_nav_tick(&n, 100 + BADGE_LEDGER_IDLE_MS - 1) == BADGE_ACTION_NONE);
    assert(badge_nav_tick(&n, 100 + BADGE_LEDGER_IDLE_MS) == BADGE_ACTION_PAGE);
    assert(n.page == BADGE_PAGE_SIGN);

    n.page = BADGE_PAGE_PAY;
    n.last_input_ms = 0;
    // 收款页闲置不调暗,到时回招牌
    assert(badge_nav_tick(&n, BADGE_DIM_IDLE_MS + 1) == BADGE_ACTION_NONE);
    assert(badge_nav_tick(&n, BADGE_PAY_IDLE_MS) == BADGE_ACTION_PAGE && n.page == BADGE_PAGE_SIGN);
    assert(badge_nav_tick(&n, BADGE_PAY_IDLE_MS + 1) == BADGE_ACTION_BRIGHTNESS && n.dimmed);
    assert(badge_nav_backlight(&n) == BADGE_DIM_PERCENT);
    assert(badge_nav_tick(&n, BADGE_PAY_IDLE_MS + 2) == BADGE_ACTION_NONE);

    // 调暗后的第一下按键只唤醒,不翻页
    assert(click(&n, BADGE_KEY_OK, BADGE_PAY_IDLE_MS + 3) == BADGE_ACTION_WAKE);
    assert(n.page == BADGE_PAGE_SIGN && !n.dimmed && badge_nav_backlight(&n) == 100);
    assert(click(&n, BADGE_KEY_OK, BADGE_PAY_IDLE_MS + 4) == BADGE_ACTION_PAGE);
}

static void test_timer_wraparound(void) {
    badge_nav_t n;
    uint32_t start = UINT32_MAX - 1000;
    badge_nav_init(&n, 1, 1, start);
    n.page = BADGE_PAGE_LEDGER;
    assert(badge_nav_tick(&n, start + 500) == BADGE_ACTION_NONE);
    assert(badge_nav_tick(&n, start + BADGE_LEDGER_IDLE_MS) == BADGE_ACTION_PAGE);

    n.page = BADGE_PAGE_LEDGER;
    assert(hold(&n, BADGE_KEY_DOWN, UINT32_MAX - 10) == BADGE_ACTION_CLEAR_ARM);
    assert(hold(&n, BADGE_KEY_DOWN, 100) == BADGE_ACTION_CLEAR);
}

int main(void) {
    test_page_cycle_and_shortcuts();
    test_in_page_selection();
    test_brightness();
    test_clear_needs_two_long_presses();
    test_idle_timeouts();
    test_timer_wraparound();
    return 0;
}
