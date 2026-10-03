#include <assert.h>
#include <string.h>

#include "ancs_proto.h"

static size_t put_attr(uint8_t *p, uint8_t id, const char *value) {
    size_t len = strlen(value);
    p[0] = id;
    p[1] = (uint8_t)len;
    p[2] = (uint8_t)(len >> 8);
    memcpy(p + 3, value, len);
    return 3 + len;
}

// 构造一条完整的 GetNotificationAttributes 应答。
static size_t response(uint8_t *buf, uint32_t uid, const char *app, const char *title,
                       const char *sub, const char *msg) {
    size_t n = 0;
    buf[n++] = 0;
    buf[n++] = (uint8_t)uid;
    buf[n++] = (uint8_t)(uid >> 8);
    buf[n++] = (uint8_t)(uid >> 16);
    buf[n++] = (uint8_t)(uid >> 24);
    n += put_attr(buf + n, 0, app);
    n += put_attr(buf + n, 1, title);
    n += put_attr(buf + n, 2, sub);
    n += put_attr(buf + n, 3, msg);
    return n;
}

static void test_parse_source(void) {
    const uint8_t ev[8] = { 0, 0x06, 4, 3, 0x78, 0x56, 0x34, 0x12 };
    ancs_source_t s;
    assert(ancs_parse_source(ev, sizeof(ev), &s));
    assert(s.event == ANCS_EVENT_ADDED && s.category == ANCS_CAT_SOCIAL && s.category_count == 3);
    assert(s.flags & ANCS_FLAG_PRE_EXISTING);
    assert(s.uid == 0x12345678);
    assert(!ancs_parse_source(ev, 7, &s));
}

static void test_build_request(void) {
    uint8_t buf[32];
    size_t n = ancs_build_get_attrs(0x01020304, buf, sizeof(buf));
    const uint8_t want[] = {
        0, 0x04, 0x03, 0x02, 0x01,
        0,                                   // App ID 不带长度
        1, ANCS_TITLE_SIZE - 1, 0,
        2, ANCS_SUBTITLE_SIZE - 1, 0,
        3, ANCS_MESSAGE_SIZE - 1, 0,
    };
    assert(n == sizeof(want));
    assert(memcmp(buf, want, n) == 0);
    assert(ancs_build_get_attrs(1, buf, n - 1) == 0);
}

static void test_reassembly_across_packets(void) {
    uint8_t buf[512];
    size_t n = response(buf, 77, "com.tencent.xin", "小林", "", "明天几点在新宿駅见?");
    // 各种切包方式都要得到同样结果,包括逐字节喂入
    size_t chunks[] = { 1, 3, 7, 20, 182 };
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        ancs_rx_t rx;
        ancs_attrs_t out;
        ancs_rx_begin(&rx, 77);
        ancs_rx_result_t r = ANCS_RX_MORE;
        for (size_t off = 0; off < n; off += chunks[c]) {
            size_t len = n - off < chunks[c] ? n - off : chunks[c];
            assert(r == ANCS_RX_MORE);
            r = ancs_rx_feed(&rx, buf + off, len, &out);
        }
        assert(r == ANCS_RX_DONE);
        assert(out.uid == 77);
        assert(strcmp(out.app_id, "com.tencent.xin") == 0);
        assert(strcmp(out.title, "小林") == 0);
        assert(out.subtitle[0] == '\0');
        assert(strcmp(out.message, "明天几点在新宿駅见?") == 0);
        // 收齐后不再接收
        assert(ancs_rx_feed(&rx, buf, 1, &out) == ANCS_RX_ERROR);
    }
}

static void test_rejects_wrong_uid_and_trailing_bytes(void) {
    uint8_t buf[256];
    size_t n = response(buf, 5, "a", "b", "c", "d");
    ancs_rx_t rx;
    ancs_attrs_t out;
    ancs_rx_begin(&rx, 6);
    assert(ancs_rx_feed(&rx, buf, n, &out) == ANCS_RX_ERROR);

    buf[n] = 0xEE;
    ancs_rx_begin(&rx, 5);
    assert(ancs_rx_feed(&rx, buf, n + 1, &out) == ANCS_RX_ERROR);

    buf[0] = 1;                                // 不是 GetNotificationAttributes 应答
    ancs_rx_begin(&rx, 5);
    assert(ancs_rx_feed(&rx, buf, n, &out) == ANCS_RX_ERROR);
}

static void test_truncates_on_utf8_boundary(void) {
    // 超长正文:iPhone 本应按请求长度截断;即使没截断,本地也不能越界,且结果是合法 UTF-8
    char long_msg[400];
    size_t len = 0;
    while (len + 3 < sizeof(long_msg) - 1) {
        memcpy(long_msg + len, "駅", 3);
        len += 3;
    }
    long_msg[len] = '\0';
    uint8_t buf[600];
    size_t n = response(buf, 9, "x", "t", "", long_msg);
    ancs_rx_t rx;
    ancs_attrs_t out;
    ancs_rx_begin(&rx, 9);
    assert(ancs_rx_feed(&rx, buf, n, &out) == ANCS_RX_DONE);
    size_t got = strlen(out.message);
    assert(got < ANCS_MESSAGE_SIZE && got % 3 == 0 && got > 0);

    // iPhone 自己切在多字节字符中间:去掉残缺的尾巴
    const char cut[] = { 'A', (char)0xE9, (char)0xA7, 0 };   // "A" + 半个 "駅"
    n = response(buf, 10, "x", cut, "", "ok");
    ancs_rx_begin(&rx, 10);
    assert(ancs_rx_feed(&rx, buf, n, &out) == ANCS_RX_DONE);
    assert(strcmp(out.title, "A") == 0);
}

static void test_uid_queue(void) {
    ancs_uidq_t q;
    ancs_uidq_init(&q);
    uint32_t uid;
    uint8_t cat;
    assert(!ancs_uidq_pop(&q, &uid, &cat));

    ancs_uidq_push(&q, 1, 4);
    ancs_uidq_push(&q, 2, 4);
    ancs_uidq_push(&q, 1, 10);                  // 重复:只更新分类
    assert(q.len == 2);
    assert(ancs_uidq_pop(&q, &uid, &cat) && uid == 1 && cat == 10);

    for (uint32_t i = 10; i < 10 + ANCS_UIDQ_SIZE + 3; i++) ancs_uidq_push(&q, i, 0);
    assert(q.len == ANCS_UIDQ_SIZE);
    assert(ancs_uidq_pop(&q, &uid, &cat) && uid == 13);   // 最旧的几条被挤掉

    ancs_uidq_remove(&q, 15);
    ancs_uidq_remove(&q, 999);
    uint32_t seen[ANCS_UIDQ_SIZE];
    size_t k = 0;
    while (ancs_uidq_pop(&q, &uid, &cat)) seen[k++] = uid;
    assert(k == ANCS_UIDQ_SIZE - 2);
    assert(seen[0] == 14 && seen[1] == 16 && seen[k - 1] == 20);
}

int main(void) {
    test_parse_source();
    test_build_request();
    test_reassembly_across_packets();
    test_rejects_wrong_uid_and_trailing_bytes();
    test_truncates_on_utf8_boundary();
    test_uid_queue();
    return 0;
}
