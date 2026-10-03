#include <assert.h>
#include <string.h>

#include "badge_fmt.h"

static void expect(uint32_t cents, bool symbol, const char *want) {
    char buf[32];
    assert(badge_fmt_cents(cents, symbol, buf, sizeof(buf)));
    assert(strcmp(buf, want) == 0);
}

int main(void) {
    expect(0, true, "\xC2\xA5" "0");
    expect(2500, true, "\xC2\xA5" "25");
    expect(1250, true, "\xC2\xA5" "12.50");
    expect(5, true, "\xC2\xA5" "0.05");
    expect(123456, false, "1,234.56");
    expect(100000000, false, "1,000,000");
    expect(999999999, true, "\xC2\xA5" "9,999,999.99");

    char small[4];
    assert(!badge_fmt_cents(12345, false, small, sizeof(small)));
    assert(small[0] == '\0');
    assert(badge_fmt_cents(999, false, small, sizeof(small)) == false);  // "9.99" 需要 5 字节
    char exact[5];
    assert(badge_fmt_cents(999, false, exact, sizeof(exact)));
    assert(strcmp(exact, "9.99") == 0);

    assert(badge_pay_is_placeholder("https://example.com/pay/wechat"));
    assert(badge_pay_is_placeholder(""));
    assert(badge_pay_is_placeholder(NULL));
    assert(!badge_pay_is_placeholder("wxp://f2f0abcdef"));
    assert(!badge_pay_is_placeholder("https://qr.alipay.com/fkx123"));
    return 0;
}
