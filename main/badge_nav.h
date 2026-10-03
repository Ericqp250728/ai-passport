// main/badge_nav.h —— 摊主胸牌的页面与按键状态机:纯逻辑,可在主机上测试。
//
// 四个页面循环:招牌 → 收款 → 记账 → 账本 → 招牌
//
//   OK 短按        下一页
//   OK 长按        招牌页:直接亮出收款码    收款页:回到招牌
//                  记账页:记一笔(选中商品)  账本页:撤销上一笔
//   上/下 短按     招牌页:调亮/调暗          收款页:切换收款码
//                  记账页:移动选中商品      账本页:无
//   下 长按 ×2     账本页:3 秒内连续两次长按才清零,防误触
//
// 隐私与省电:账本页闲置 15 秒、收款页闲置 120 秒自动回到招牌;
// 非收款页闲置 60 秒后背光调暗,任意按键恢复(恢复的那一下只唤醒,不执行动作)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BADGE_PAGE_SIGN = 0,
    BADGE_PAGE_PAY,
    BADGE_PAGE_SELL,
    BADGE_PAGE_LEDGER,
    BADGE_PAGE_COUNT,
} badge_page_t;

typedef enum {
    BADGE_KEY_UP = 0,
    BADGE_KEY_DOWN,
    BADGE_KEY_OK,
} badge_key_t;

typedef enum {
    BADGE_PRESS_CLICK = 0,
    BADGE_PRESS_LONG,
} badge_press_t;

typedef enum {
    BADGE_ACTION_NONE = 0,
    BADGE_ACTION_PAGE,         // 页面变了:整页重建
    BADGE_ACTION_SELECT,       // 页内选中项/收款码变了:局部刷新
    BADGE_ACTION_BRIGHTNESS,   // 背光档位变了
    BADGE_ACTION_WAKE,         // 只是把调暗的屏幕唤醒
    BADGE_ACTION_RECORD,       // 记一笔:商品为 nav.sell_index
    BADGE_ACTION_UNDO,         // 撤销上一笔
    BADGE_ACTION_CLEAR_ARM,    // 清零已待确认:提示再长按一次
    BADGE_ACTION_CLEAR_DISARM, // 清零确认超时:收回提示
    BADGE_ACTION_CLEAR,        // 清零本场账本
} badge_action_t;

#define BADGE_BRIGHTNESS_LEVELS     3
#define BADGE_LEDGER_IDLE_MS    15000u
#define BADGE_PAY_IDLE_MS      120000u
#define BADGE_DIM_IDLE_MS       60000u
#define BADGE_CLEAR_CONFIRM_MS   3000u
#define BADGE_DIM_PERCENT           10

typedef struct {
    badge_page_t page;
    size_t pay_index;
    size_t pay_count;
    size_t sell_index;
    size_t item_count;
    uint8_t brightness;        // 0..BADGE_BRIGHTNESS_LEVELS-1
    bool dimmed;
    bool clear_armed;
    uint32_t clear_deadline_ms;
    uint32_t last_input_ms;
} badge_nav_t;

void badge_nav_init(badge_nav_t *nav, size_t pay_count, size_t item_count, uint32_t now_ms);

badge_action_t badge_nav_input(badge_nav_t *nav, badge_key_t key, badge_press_t press,
                               uint32_t now_ms);

// 周期调用(约每秒一次)。返回需要执行的超时动作;一次只返回一个,其余留给下次调用。
badge_action_t badge_nav_tick(badge_nav_t *nav, uint32_t now_ms);

// 当前应使用的背光百分比:收款页始终 100%(方便扫码),调暗时为 BADGE_DIM_PERCENT。
uint8_t badge_nav_backlight(const badge_nav_t *nav);
