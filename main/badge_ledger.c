// main/badge_ledger.c —— 本场账本的记账、撤销与持久化编码。
#include "badge_ledger.h"

#include <string.h>

#define LEDGER_MAGIC   0x4C474442u   // "BDGL"
#define LEDGER_VERSION 1u

void badge_ledger_reset(badge_ledger_t *ledger) {
    memset(ledger, 0, sizeof(*ledger));
}

bool badge_ledger_record(badge_ledger_t *ledger, size_t item, uint32_t cents) {
    if (item >= BADGE_MAX_ITEMS || cents == 0) return false;
    if (cents > BADGE_MAX_TOTAL_CENTS - ledger->total_cents) return false;
    if (ledger->item_count[item] == UINT16_MAX) return false;

    ledger->total_cents += cents;
    ledger->sales++;
    ledger->item_cents[item] += cents;
    ledger->item_count[item]++;

    ledger->undo_item[ledger->undo_head] = (uint8_t)item;
    ledger->undo_cents[ledger->undo_head] = cents;
    ledger->undo_head = (uint8_t)((ledger->undo_head + 1) % BADGE_UNDO_DEPTH);
    if (ledger->undo_len < BADGE_UNDO_DEPTH) ledger->undo_len++;
    return true;
}

bool badge_ledger_undo(badge_ledger_t *ledger, size_t *item_out, uint32_t *cents_out) {
    if (ledger->undo_len == 0) return false;
    uint8_t slot = (uint8_t)((ledger->undo_head + BADGE_UNDO_DEPTH - 1) % BADGE_UNDO_DEPTH);
    uint8_t item = ledger->undo_item[slot];
    uint32_t cents = ledger->undo_cents[slot];

    ledger->total_cents -= cents;
    ledger->sales--;
    ledger->item_cents[item] -= cents;
    ledger->item_count[item]--;

    ledger->undo_item[slot] = 0;
    ledger->undo_cents[slot] = 0;
    ledger->undo_head = slot;
    ledger->undo_len--;
    if (item_out) *item_out = item;
    if (cents_out) *cents_out = cents;
    return true;
}

bool badge_ledger_valid(const badge_ledger_t *ledger) {
    uint64_t cents = 0;
    uint64_t sales = 0;
    for (size_t i = 0; i < BADGE_MAX_ITEMS; i++) {
        if (ledger->item_count[i] == 0 && ledger->item_cents[i] != 0) return false;
        cents += ledger->item_cents[i];
        sales += ledger->item_count[i];
    }
    if (cents != ledger->total_cents || sales != ledger->sales) return false;
    if (ledger->total_cents > BADGE_MAX_TOTAL_CENTS) return false;
    if (ledger->undo_head >= BADGE_UNDO_DEPTH || ledger->undo_len > BADGE_UNDO_DEPTH) return false;
    if (ledger->undo_len > ledger->sales) return false;

    // 撤销记录必须能从对应商品里扣得下,否则撤销会让计数下溢。
    uint32_t pending_count[BADGE_MAX_ITEMS] = {0};
    uint64_t pending_cents[BADGE_MAX_ITEMS] = {0};
    for (uint8_t n = 0; n < ledger->undo_len; n++) {
        uint8_t slot = (uint8_t)((ledger->undo_head + BADGE_UNDO_DEPTH - 1 - n) % BADGE_UNDO_DEPTH);
        uint8_t item = ledger->undo_item[slot];
        if (item >= BADGE_MAX_ITEMS || ledger->undo_cents[slot] == 0) return false;
        pending_count[item]++;
        pending_cents[item] += ledger->undo_cents[slot];
    }
    for (size_t i = 0; i < BADGE_MAX_ITEMS; i++) {
        if (pending_count[i] > ledger->item_count[i]) return false;
        if (pending_cents[i] > ledger->item_cents[i]) return false;
    }
    return true;
}

static uint32_t fnv1a(const uint8_t *data, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint8_t *put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint8_t *put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return p + 4;
}

static const uint8_t *get_u16(const uint8_t *p, uint16_t *v) {
    *v = (uint16_t)(p[0] | (p[1] << 8));
    return p + 2;
}

static const uint8_t *get_u32(const uint8_t *p, uint32_t *v) {
    *v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return p + 4;
}

// 布局:magic u32 | version u32 | total u32 | sales u32 | item_cents u32[8] | item_count u16[8]
//       | undo_item u8[32] | undo_cents u32[32] | undo_head u8 | undo_len u8 | fnv1a u32
#define PAYLOAD_SIZE (4 + 4 + 4 + 4 + 4 * BADGE_MAX_ITEMS + 2 * BADGE_MAX_ITEMS \
                      + BADGE_UNDO_DEPTH + 4 * BADGE_UNDO_DEPTH + 1 + 1)
#define BLOB_SIZE (PAYLOAD_SIZE + 4)

_Static_assert(BLOB_SIZE <= BADGE_LEDGER_BLOB_SIZE, "ledger blob grew past its NVS budget");

size_t badge_ledger_serialize(const badge_ledger_t *ledger, uint8_t *buf, size_t cap) {
    if (cap < BLOB_SIZE) return 0;
    uint8_t *p = buf;
    p = put_u32(p, LEDGER_MAGIC);
    p = put_u32(p, LEDGER_VERSION);
    p = put_u32(p, ledger->total_cents);
    p = put_u32(p, ledger->sales);
    for (size_t i = 0; i < BADGE_MAX_ITEMS; i++) p = put_u32(p, ledger->item_cents[i]);
    for (size_t i = 0; i < BADGE_MAX_ITEMS; i++) p = put_u16(p, ledger->item_count[i]);
    memcpy(p, ledger->undo_item, BADGE_UNDO_DEPTH);
    p += BADGE_UNDO_DEPTH;
    for (size_t i = 0; i < BADGE_UNDO_DEPTH; i++) p = put_u32(p, ledger->undo_cents[i]);
    *p++ = ledger->undo_head;
    *p++ = ledger->undo_len;
    p = put_u32(p, fnv1a(buf, PAYLOAD_SIZE));
    return (size_t)(p - buf);
}

bool badge_ledger_deserialize(badge_ledger_t *ledger, const uint8_t *buf, size_t len) {
    if (len != BLOB_SIZE) return false;
    uint32_t magic, version, checksum;
    get_u32(buf + PAYLOAD_SIZE, &checksum);
    if (checksum != fnv1a(buf, PAYLOAD_SIZE)) return false;

    const uint8_t *p = buf;
    p = get_u32(p, &magic);
    p = get_u32(p, &version);
    if (magic != LEDGER_MAGIC || version != LEDGER_VERSION) return false;

    badge_ledger_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    p = get_u32(p, &parsed.total_cents);
    p = get_u32(p, &parsed.sales);
    for (size_t i = 0; i < BADGE_MAX_ITEMS; i++) p = get_u32(p, &parsed.item_cents[i]);
    for (size_t i = 0; i < BADGE_MAX_ITEMS; i++) p = get_u16(p, &parsed.item_count[i]);
    memcpy(parsed.undo_item, p, BADGE_UNDO_DEPTH);
    p += BADGE_UNDO_DEPTH;
    for (size_t i = 0; i < BADGE_UNDO_DEPTH; i++) p = get_u32(p, &parsed.undo_cents[i]);
    parsed.undo_head = *p++;
    parsed.undo_len = *p++;

    if (!badge_ledger_valid(&parsed)) return false;
    *ledger = parsed;
    return true;
}
