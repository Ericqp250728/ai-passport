// main/badge_nav.c —— 页面与按键状态机实现。时间用 uint32 毫秒,差值运算可安全跨越回绕。
#include "badge_nav.h"

static const uint8_t BRIGHTNESS_PERCENT[BADGE_BRIGHTNESS_LEVELS] = { 40, 70, 100 };

static bool elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t span_ms) {
    return (uint32_t)(now_ms - since_ms) >= span_ms;
}

static bool reached(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static badge_action_t go_page(badge_nav_t *nav, badge_page_t page) {
    nav->page = page;
    nav->clear_armed = false;
    return BADGE_ACTION_PAGE;
}

void badge_nav_init(badge_nav_t *nav, size_t pay_count, size_t item_count, uint32_t now_ms) {
    *nav = (badge_nav_t){
        .page = BADGE_PAGE_SIGN,
        .pay_count = pay_count,
        .item_count = item_count,
        .brightness = BADGE_BRIGHTNESS_LEVELS - 1,
        .last_input_ms = now_ms,
    };
}

static badge_action_t step(size_t *index, size_t count, bool forward) {
    if (count < 2) return BADGE_ACTION_NONE;
    *index = forward ? (*index + 1) % count : (*index + count - 1) % count;
    return BADGE_ACTION_SELECT;
}

static badge_action_t on_long(badge_nav_t *nav, badge_key_t key) {
    if (key == BADGE_KEY_OK) {
        switch (nav->page) {
        case BADGE_PAGE_SIGN:   return go_page(nav, BADGE_PAGE_PAY);
        case BADGE_PAGE_PAY:    return go_page(nav, BADGE_PAGE_SIGN);
        case BADGE_PAGE_SELL:   return nav->item_count ? BADGE_ACTION_RECORD : BADGE_ACTION_NONE;
        case BADGE_PAGE_LEDGER: return BADGE_ACTION_UNDO;
        default:                return BADGE_ACTION_NONE;
        }
    }
    if (key == BADGE_KEY_DOWN && nav->page == BADGE_PAGE_LEDGER) {
        if (nav->clear_armed) {
            nav->clear_armed = false;
            return BADGE_ACTION_CLEAR;
        }
        nav->clear_armed = true;
        nav->clear_deadline_ms = nav->last_input_ms + BADGE_CLEAR_CONFIRM_MS;
        return BADGE_ACTION_CLEAR_ARM;
    }
    return BADGE_ACTION_NONE;
}

static badge_action_t on_click(badge_nav_t *nav, badge_key_t key) {
    if (key == BADGE_KEY_OK) {
        return go_page(nav, (badge_page_t)((nav->page + 1) % BADGE_PAGE_COUNT));
    }
    bool down = key == BADGE_KEY_DOWN;
    switch (nav->page) {
    case BADGE_PAGE_SIGN:
        if (down && nav->brightness > 0) {
            nav->brightness--;
            return BADGE_ACTION_BRIGHTNESS;
        }
        if (!down && nav->brightness + 1 < BADGE_BRIGHTNESS_LEVELS) {
            nav->brightness++;
            return BADGE_ACTION_BRIGHTNESS;
        }
        return BADGE_ACTION_NONE;
    case BADGE_PAGE_PAY:
        return step(&nav->pay_index, nav->pay_count, down);
    case BADGE_PAGE_SELL:
        return step(&nav->sell_index, nav->item_count, down);
    default:
        return BADGE_ACTION_NONE;
    }
}

badge_action_t badge_nav_input(badge_nav_t *nav, badge_key_t key, badge_press_t press,
                               uint32_t now_ms) {
    nav->last_input_ms = now_ms;
    if (nav->dimmed) {
        nav->dimmed = false;
        return BADGE_ACTION_WAKE;
    }
    // 清零只接受确认窗口内"紧接着的下一次长按↓";其他按键或超时都收回待确认状态,
    // 超时后的长按↓会重新进入待确认,而不是直接清零。
    bool was_armed = nav->clear_armed;
    bool confirms = was_armed && press == BADGE_PRESS_LONG && key == BADGE_KEY_DOWN
                 && !reached(now_ms, nav->clear_deadline_ms);
    if (was_armed && !confirms) nav->clear_armed = false;

    badge_action_t action = press == BADGE_PRESS_LONG ? on_long(nav, key) : on_click(nav, key);
    if (was_armed && !confirms && action == BADGE_ACTION_NONE) {
        return BADGE_ACTION_CLEAR_DISARM;
    }
    return action;
}

badge_action_t badge_nav_tick(badge_nav_t *nav, uint32_t now_ms) {
    if (nav->clear_armed && reached(now_ms, nav->clear_deadline_ms)) {
        nav->clear_armed = false;
        return BADGE_ACTION_CLEAR_DISARM;
    }
    if (nav->page == BADGE_PAGE_LEDGER && elapsed(now_ms, nav->last_input_ms, BADGE_LEDGER_IDLE_MS)) {
        return go_page(nav, BADGE_PAGE_SIGN);
    }
    if (nav->page == BADGE_PAGE_PAY && elapsed(now_ms, nav->last_input_ms, BADGE_PAY_IDLE_MS)) {
        return go_page(nav, BADGE_PAGE_SIGN);
    }
    if (!nav->dimmed && nav->page != BADGE_PAGE_PAY
        && elapsed(now_ms, nav->last_input_ms, BADGE_DIM_IDLE_MS)) {
        nav->dimmed = true;
        return BADGE_ACTION_BRIGHTNESS;
    }
    return BADGE_ACTION_NONE;
}

uint8_t badge_nav_backlight(const badge_nav_t *nav) {
    if (nav->page == BADGE_PAGE_PAY) return 100;
    if (nav->dimmed) return BADGE_DIM_PERCENT;
    return BRIGHTNESS_PERCENT[nav->brightness];
}
