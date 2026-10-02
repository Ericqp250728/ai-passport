// main/badge_fmt.h —— 金额格式化等纯函数,可在主机上测试。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 分 → "¥12" / "¥12.50" / "¥1,234.05"。整元不显示小数,其余固定两位小数。
// with_symbol=false 时不带 "¥"。cap 不足时写入空串并返回 false。
bool badge_fmt_cents(uint32_t cents, bool with_symbol, char *out, size_t cap);

// 收款内容是否仍是仓库自带的示例占位(界面据此提醒"请替换")。
bool badge_pay_is_placeholder(const char *payload);
