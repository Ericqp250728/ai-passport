// main/badge_fmt.c —— 金额格式化。
#include "badge_fmt.h"

#include <string.h>

#define YUAN_SIGN "\xC2\xA5"   // U+00A5 "¥" 的 UTF-8 编码

bool badge_fmt_cents(uint32_t cents, bool with_symbol, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';

    uint32_t yuan = cents / 100;
    uint32_t fen = cents % 100;

    // 先从个位往前写出带千分位的整数部分。
    char digits[16];
    size_t n = 0;
    int group = 0;
    do {
        if (group == 3) {
            digits[n++] = ',';
            group = 0;
        }
        digits[n++] = (char)('0' + yuan % 10);
        yuan /= 10;
        group++;
    } while (yuan > 0);

    char buf[32];
    size_t len = 0;
    if (with_symbol) {
        memcpy(buf, YUAN_SIGN, 2);
        len = 2;
    }
    while (n > 0) buf[len++] = digits[--n];
    if (fen != 0) {
        buf[len++] = '.';
        buf[len++] = (char)('0' + fen / 10);
        buf[len++] = (char)('0' + fen % 10);
    }
    buf[len] = '\0';

    if (len + 1 > cap) return false;
    memcpy(out, buf, len + 1);
    return true;
}

bool badge_pay_is_placeholder(const char *payload) {
    return payload == NULL || payload[0] == '\0' || strstr(payload, "example.com") != NULL;
}
