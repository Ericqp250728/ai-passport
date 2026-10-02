// main/badge_content.c —— 示例内容。复制为 badge_content_local.c 后改成你自己的,
// 见 badge_content.h 顶部说明。下面的收款内容是占位,界面会提示"示例码,请替换"。
#include "badge_content.h"

const badge_content_t BADGE_CONTENT = {
    .shop = "小林手作",
    .slogan = "手工饰品 · 现做现卖",
    .lines = {
        "耳环 / 手链 / 发夹",
        "支持现场定制刻字",
        "微信支付宝都能付",
    },
    .line_count = 3,
    .pay = {
        { .label = "微信", .payload = "https://example.com/pay/wechat" },
        { .label = "支付宝", .payload = "https://example.com/pay/alipay" },
    },
    .pay_count = 2,
    .items = {
        { .name = "耳环", .price_cents = 2500 },
        { .name = "手链", .price_cents = 3500 },
        { .name = "发夹", .price_cents = 1500 },
        { .name = "定制刻字", .price_cents = 1000 },
        { .name = "礼品包装", .price_cents = 200 },
    },
    .item_count = 5,
};
