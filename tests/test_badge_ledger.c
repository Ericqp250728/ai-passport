#include <assert.h>
#include <string.h>

#include "badge_ledger.h"

static void test_record_and_undo(void) {
    badge_ledger_t l;
    badge_ledger_reset(&l);
    assert(badge_ledger_valid(&l));

    assert(badge_ledger_record(&l, 0, 2500));
    assert(badge_ledger_record(&l, 2, 1500));
    assert(badge_ledger_record(&l, 0, 2500));
    assert(l.total_cents == 6500 && l.sales == 3);
    assert(l.item_count[0] == 2 && l.item_cents[0] == 5000);
    assert(badge_ledger_valid(&l));

    size_t item;
    uint32_t cents;
    assert(badge_ledger_undo(&l, &item, &cents));
    assert(item == 0 && cents == 2500);
    assert(l.total_cents == 4000 && l.sales == 2 && l.item_count[0] == 1);
    assert(badge_ledger_undo(&l, &item, &cents));
    assert(item == 2 && cents == 1500);
    assert(badge_ledger_undo(&l, &item, &cents));
    assert(l.total_cents == 0 && l.sales == 0);
    assert(!badge_ledger_undo(&l, &item, &cents));
    assert(badge_ledger_valid(&l));
}

static void test_rejects_invalid_records(void) {
    badge_ledger_t l;
    badge_ledger_reset(&l);
    assert(!badge_ledger_record(&l, BADGE_MAX_ITEMS, 100));
    assert(!badge_ledger_record(&l, 0, 0));
    assert(badge_ledger_record(&l, 1, BADGE_MAX_TOTAL_CENTS));
    assert(!badge_ledger_record(&l, 1, 1));          // 总额封顶
    assert(l.sales == 1 && l.total_cents == BADGE_MAX_TOTAL_CENTS);

    badge_ledger_reset(&l);
    l.item_count[3] = UINT16_MAX - 1;
    l.item_cents[3] = UINT16_MAX - 1;
    l.total_cents = UINT16_MAX - 1;
    l.sales = UINT16_MAX - 1;
    assert(badge_ledger_record(&l, 3, 1));
    assert(!badge_ledger_record(&l, 3, 1));          // 单品计数封顶
}

static void test_undo_depth_is_bounded(void) {
    badge_ledger_t l;
    badge_ledger_reset(&l);
    for (int i = 0; i < BADGE_UNDO_DEPTH + 5; i++) {
        assert(badge_ledger_record(&l, (size_t)(i % 3), 100 + (uint32_t)i));
    }
    assert(l.undo_len == BADGE_UNDO_DEPTH);
    size_t item;
    uint32_t cents;
    // 最后一笔先撤销
    assert(badge_ledger_undo(&l, &item, &cents));
    assert(cents == 100 + BADGE_UNDO_DEPTH + 4);
    int undone = 1;
    while (badge_ledger_undo(&l, &item, &cents)) undone++;
    assert(undone == BADGE_UNDO_DEPTH);
    // 超出撤销深度的前 5 笔留在账上
    assert(l.sales == 5);
    assert(l.total_cents == 100 + 101 + 102 + 103 + 104);
    assert(badge_ledger_valid(&l));
}

static void test_serialize_round_trip(void) {
    badge_ledger_t a, b;
    badge_ledger_reset(&a);
    for (int i = 0; i < 40; i++) assert(badge_ledger_record(&a, (size_t)(i % 5), 250));
    size_t item;
    assert(badge_ledger_undo(&a, &item, NULL));

    uint8_t blob[BADGE_LEDGER_BLOB_SIZE];
    size_t len = badge_ledger_serialize(&a, blob, sizeof(blob));
    assert(len > 0 && len <= sizeof(blob));
    assert(badge_ledger_serialize(&a, blob, len - 1) == 0);

    memset(&b, 0xAA, sizeof(b));
    assert(badge_ledger_deserialize(&b, blob, len));
    assert(memcmp(&a, &b, sizeof(a)) == 0);

    // 撤销功能在恢复后依然可用
    uint32_t cents;
    assert(badge_ledger_undo(&b, &item, &cents) && cents == 250);
}

static void test_deserialize_rejects_corruption(void) {
    badge_ledger_t a, b;
    badge_ledger_reset(&a);
    assert(badge_ledger_record(&a, 1, 999));
    uint8_t blob[BADGE_LEDGER_BLOB_SIZE];
    size_t len = badge_ledger_serialize(&a, blob, sizeof(blob));

    badge_ledger_reset(&b);
    assert(badge_ledger_record(&b, 4, 7));
    badge_ledger_t before = b;

    uint8_t bad[BADGE_LEDGER_BLOB_SIZE];
    memcpy(bad, blob, len);
    bad[10] ^= 0x01;                                  // 翻转一位:校验和不符
    assert(!badge_ledger_deserialize(&b, bad, len));
    assert(!badge_ledger_deserialize(&b, blob, len - 1));   // 长度不符
    assert(memcmp(&b, &before, sizeof(b)) == 0);      // 失败时不改动目标
}

static void test_valid_rejects_inconsistent_state(void) {
    badge_ledger_t l;
    badge_ledger_reset(&l);
    assert(badge_ledger_record(&l, 0, 100));
    l.total_cents = 101;
    assert(!badge_ledger_valid(&l));

    badge_ledger_reset(&l);
    assert(badge_ledger_record(&l, 0, 100));
    l.undo_item[0] = 5;                               // 撤销记录指向没卖过的商品
    assert(!badge_ledger_valid(&l));

    badge_ledger_reset(&l);
    l.undo_len = 1;                                   // 撤销条数多于笔数
    assert(!badge_ledger_valid(&l));
}

int main(void) {
    test_record_and_undo();
    test_rejects_invalid_records();
    test_undo_depth_is_bounded();
    test_serialize_round_trip();
    test_deserialize_rejects_corruption();
    test_valid_rejects_inconsistent_state();
    return 0;
}
