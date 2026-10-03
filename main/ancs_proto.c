// main/ancs_proto.c —— ANCS 事件解析、属性请求与 Data Source 流式拼包。
#include "ancs_proto.h"

#include <string.h>

#define CMD_GET_NOTIFICATION_ATTRIBUTES 0
#define ATTR_APP_ID   0
#define ATTR_TITLE    1
#define ATTR_SUBTITLE 2
#define ATTR_MESSAGE  3
#define ATTR_COUNT    4

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool ancs_parse_source(const uint8_t *data, size_t len, ancs_source_t *out) {
    if (!data || len < 8) return false;
    out->event = data[0];
    out->flags = data[1];
    out->category = data[2];
    out->category_count = data[3];
    out->uid = get_u32(data + 4);
    return true;
}

size_t ancs_build_get_attrs(uint32_t uid, uint8_t *buf, size_t cap) {
    static const struct {
        uint8_t id;
        uint16_t max;   // 0 = 该属性不带长度参数
    } REQ[ATTR_COUNT] = {
        { ATTR_APP_ID, 0 },
        { ATTR_TITLE, ANCS_TITLE_SIZE - 1 },
        { ATTR_SUBTITLE, ANCS_SUBTITLE_SIZE - 1 },
        { ATTR_MESSAGE, ANCS_MESSAGE_SIZE - 1 },
    };
    size_t need = 1 + 4;
    for (size_t i = 0; i < ATTR_COUNT; i++) need += REQ[i].max ? 3 : 1;
    if (cap < need) return 0;

    size_t n = 0;
    buf[n++] = CMD_GET_NOTIFICATION_ATTRIBUTES;
    buf[n++] = (uint8_t)uid;
    buf[n++] = (uint8_t)(uid >> 8);
    buf[n++] = (uint8_t)(uid >> 16);
    buf[n++] = (uint8_t)(uid >> 24);
    for (size_t i = 0; i < ATTR_COUNT; i++) {
        buf[n++] = REQ[i].id;
        if (REQ[i].max) {
            buf[n++] = (uint8_t)REQ[i].max;
            buf[n++] = (uint8_t)(REQ[i].max >> 8);
        }
    }
    return n;
}

// 截掉末尾不完整的 UTF-8 序列(iPhone 按字节截断时可能切在多字节字符中间)。
static void trim_utf8_tail(char *s, size_t len) {
    size_t i = len;
    size_t back = 0;
    while (i > 0 && back < 4 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) {
        i--;
        back++;
    }
    if (i == 0) {
        s[len] = '\0';
        return;
    }
    uint8_t lead = (uint8_t)s[i - 1];
    size_t want = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3
                : (lead & 0xF8) == 0xF0 ? 4 : 0;
    size_t have = back + 1;
    if (lead < 0x80) {
        s[len] = '\0';
    } else if (want == 0 || have < want) {
        s[i - 1] = '\0';         // 不完整或非法的首字节:整段丢弃
    } else {
        s[len] = '\0';
    }
}

static char *field_for(ancs_attrs_t *a, uint8_t id, size_t *size) {
    switch (id) {
    case ATTR_APP_ID:   *size = sizeof(a->app_id); return a->app_id;
    case ATTR_TITLE:    *size = sizeof(a->title); return a->title;
    case ATTR_SUBTITLE: *size = sizeof(a->subtitle); return a->subtitle;
    case ATTR_MESSAGE:  *size = sizeof(a->message); return a->message;
    default:            *size = 0; return NULL;
    }
}

static void finish_attr(ancs_rx_t *rx) {
    size_t size;
    char *field = field_for(&rx->attrs, rx->attr_id, &size);
    if (field) {
        size_t stored = rx->attr_pos < size - 1 ? rx->attr_pos : size - 1;
        trim_utf8_tail(field, stored);
    }
    rx->attr_index++;
    rx->stage = 0;
}

void ancs_rx_begin(ancs_rx_t *rx, uint32_t uid) {
    memset(rx, 0, sizeof(*rx));
    rx->expect_uid = uid;
    rx->active = true;
    rx->attrs.uid = uid;
}

ancs_rx_result_t ancs_rx_feed(ancs_rx_t *rx, const uint8_t *data, size_t len, ancs_attrs_t *out) {
    if (!rx->active) return ANCS_RX_ERROR;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (rx->header_len < sizeof(rx->header)) {
            rx->header[rx->header_len++] = b;
            if (rx->header_len == sizeof(rx->header)) {
                if (rx->header[0] != CMD_GET_NOTIFICATION_ATTRIBUTES
                    || get_u32(rx->header + 1) != rx->expect_uid) {
                    rx->active = false;
                    return ANCS_RX_ERROR;
                }
            }
            continue;
        }
        switch (rx->stage) {
        case 0:
            rx->attr_id = b;
            rx->stage = 1;
            break;
        case 1:
            rx->attr_len = b;
            rx->stage = 2;
            break;
        case 2:
            rx->attr_len |= (uint16_t)(b << 8);
            rx->attr_pos = 0;
            rx->stage = 3;
            if (rx->attr_len == 0) finish_attr(rx);
            break;
        default: {
            size_t size;
            char *field = field_for(&rx->attrs, rx->attr_id, &size);
            if (field && rx->attr_pos < size - 1) field[rx->attr_pos] = (char)b;
            rx->attr_pos++;
            if (rx->attr_pos == rx->attr_len) finish_attr(rx);
            break;
        }
        }
        if (rx->attr_index == ATTR_COUNT) {
            rx->active = false;
            // 收齐后不应再有多余字节
            if (i + 1 != len) return ANCS_RX_ERROR;
            *out = rx->attrs;
            return ANCS_RX_DONE;
        }
    }
    return ANCS_RX_MORE;
}

// ---------------------------------------------------------------- UID 队列

void ancs_uidq_init(ancs_uidq_t *q) {
    memset(q, 0, sizeof(*q));
}

static uint8_t slot(const ancs_uidq_t *q, uint8_t n) {
    return (uint8_t)((q->head + n) % ANCS_UIDQ_SIZE);
}

void ancs_uidq_push(ancs_uidq_t *q, uint32_t uid, uint8_t category) {
    for (uint8_t n = 0; n < q->len; n++) {
        if (q->uid[slot(q, n)] == uid) {
            q->category[slot(q, n)] = category;
            return;
        }
    }
    if (q->len == ANCS_UIDQ_SIZE) {
        q->head = slot(q, 1);   // 挤掉最旧的
        q->len--;
    }
    uint8_t s = slot(q, q->len);
    q->uid[s] = uid;
    q->category[s] = category;
    q->len++;
}

bool ancs_uidq_pop(ancs_uidq_t *q, uint32_t *uid, uint8_t *category) {
    if (q->len == 0) return false;
    *uid = q->uid[q->head];
    *category = q->category[q->head];
    q->head = slot(q, 1);
    q->len--;
    return true;
}

void ancs_uidq_remove(ancs_uidq_t *q, uint32_t uid) {
    for (uint8_t n = 0; n < q->len; n++) {
        if (q->uid[slot(q, n)] != uid) continue;
        for (uint8_t m = n; m + 1 < q->len; m++) {
            q->uid[slot(q, m)] = q->uid[slot(q, m + 1)];
            q->category[slot(q, m)] = q->category[slot(q, m + 1)];
        }
        q->len--;
        return;
    }
}
