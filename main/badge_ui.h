// main/badge_ui.h —— 摊主胸牌界面。所有函数都必须在持有 bsp_lvgl_lock() 时调用。
#pragma once

#include <stdbool.h>

#include "badge_content.h"
#include "badge_ledger.h"
#include "badge_nav.h"

// 创建屏幕、顶部状态栏(页码点 + 电量)与提示浮层。只调用一次。
void badge_ui_init(const badge_content_t *content);

// 整页重建(切页时)。
void badge_ui_show_page(const badge_nav_t *nav, const badge_ledger_t *ledger);

// 当前页局部刷新:选中项、收款码切换、账本数字、清零确认提示。
void badge_ui_refresh(const badge_nav_t *nav, const badge_ledger_t *ledger);

// 底部浮层提示,约 1.5 秒后自动消失。error=true 时用警示色。
void badge_ui_toast(const char *text, bool error);

// 电量百分比;-1 表示读不到,此时隐藏电量而不是显示假数字。
void badge_ui_set_battery(int soc);
