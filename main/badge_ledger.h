// main/badge_ledger.h —— 摊主胸牌的"本场账本":纯逻辑,不依赖 ESP-IDF/LVGL,可在主机上测试。
//
// 设备没有可靠的墙上时钟(不联网就不知道今天几号),所以账本按"本场"计:
// 从上次清零开始累计,由摊主在收摊时手动清零。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BADGE_MAX_ITEMS   8
#define BADGE_UNDO_DEPTH  32
// 单场营业额上限 ¥9,999,999.99;超过即拒绝记账,避免 uint32 溢出与界面放不下。
#define BADGE_MAX_TOTAL_CENTS 999999999u
#define BADGE_LEDGER_BLOB_SIZE 256

typedef struct {
    uint32_t total_cents;                       // 本场总收入(分)
    uint32_t sales;                             // 本场总笔数
    uint32_t item_cents[BADGE_MAX_ITEMS];       // 各商品收入(按记账时的价格累计)
    uint16_t item_count[BADGE_MAX_ITEMS];       // 各商品笔数
    uint8_t  undo_item[BADGE_UNDO_DEPTH];       // 最近若干笔,环形缓冲,用于撤销
    uint32_t undo_cents[BADGE_UNDO_DEPTH];
    uint8_t  undo_head;                         // 下一条写入位置
    uint8_t  undo_len;                          // 可撤销条数
} badge_ledger_t;

void badge_ledger_reset(badge_ledger_t *ledger);

// 记一笔。item 越界、价格为 0、计数或总额溢出时返回 false 且账本不变。
bool badge_ledger_record(badge_ledger_t *ledger, size_t item, uint32_t cents);

// 撤销最近一笔(最多回溯 BADGE_UNDO_DEPTH 笔)。没有可撤销记录时返回 false。
bool badge_ledger_undo(badge_ledger_t *ledger, size_t *item_out, uint32_t *cents_out);

// 账本是否内部自洽(各项之和等于总数、撤销记录合法)。反序列化后据此拒绝损坏数据。
bool badge_ledger_valid(const badge_ledger_t *ledger);

// 固定小端布局 + 版本 + 校验和,不直接 memcpy 结构体(避免填充字节与对齐差异)。
// 返回写入字节数;cap 不足时返回 0。
size_t badge_ledger_serialize(const badge_ledger_t *ledger, uint8_t *buf, size_t cap);

// 版本、长度、校验和或自洽性任一不符都返回 false,且不修改 *ledger。
bool badge_ledger_deserialize(badge_ledger_t *ledger, const uint8_t *buf, size_t len);
