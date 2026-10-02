// main/badge_content.h —— 胸牌上显示的内容(店名、介绍、收款码、价目)。
//
// 仓库里的 badge_content.c 只是示例。要换成你自己的内容:
//   1. 复制 main/badge_content.c 为 main/badge_content_local.c 并修改
//      (该文件已被 .gitignore 忽略,收款码等个人信息不会被提交)。
//   2. 运行 python3 tools/gen_badge_fonts.py 重新生成字库,
//      否则新出现的汉字会显示成方框。
//   3. 重新编译烧录。构建时若存在 badge_content_local.c 就用它代替示例。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "badge_ledger.h"

#define BADGE_MAX_LINES 4
#define BADGE_MAX_PAY   2

typedef struct {
    const char *name;          // 商品名,建议不超过 6 个汉字
    uint32_t price_cents;      // 价格(分):¥12.50 写 1250
} badge_item_t;

typedef struct {
    const char *label;         // 显示在二维码下方,如 "微信" "支付宝"
    // 二维码内容。用任意扫码工具扫你的收款码得到这串文字(微信通常以 wxp:// 开头,
    // 支付宝通常以 https://qr.alipay.com/ 开头),原样粘贴到这里。
    const char *payload;
} badge_pay_t;

typedef struct {
    const char *shop;                       // 店名/昵称,建议不超过 7 个汉字
    const char *slogan;                     // 一句话介绍
    const char *lines[BADGE_MAX_LINES];     // 招牌页的卖点,每行建议不超过 8 个汉字(超出显示省略号)
    size_t line_count;
    badge_pay_t pay[BADGE_MAX_PAY];
    size_t pay_count;
    badge_item_t items[BADGE_MAX_ITEMS];
    size_t item_count;
} badge_content_t;

extern const badge_content_t BADGE_CONTENT;
