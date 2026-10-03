// main/notif_model.c —— 通知列表与文本清洗。
#include "notif_model.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *bundle;
    const char *name;
} app_name_t;

// 旅行中最常见的 App。包名区分大小写。
static const app_name_t APP_NAMES[] = {
    { "com.tencent.xin", "微信" },
    { "jp.naver.line", "LINE" },
    { "com.apple.MobileSMS", "短信" },
    { "com.apple.mobilephone", "电话" },
    { "com.apple.facetime", "FaceTime" },
    { "com.apple.mobilemail", "邮件" },
    { "com.apple.mobilecal", "日历" },
    { "com.apple.reminders", "提醒" },
    { "com.apple.Maps", "地图" },
    { "com.google.Maps", "谷歌地图" },
    { "com.autonavi.amapauto", "高德" },
    { "com.autonavi.minimap", "高德地图" },
    { "com.baidu.map", "百度地图" },
    { "com.tencent.mqq", "QQ" },
    { "com.alipay.iphoneclient", "支付宝" },
    { "com.ss.iphone.ugc.Aweme", "抖音" },
    { "com.xingin.discover", "小红书" },
    { "net.whatsapp.WhatsApp", "WhatsApp" },
    { "com.burbn.instagram", "Instagram" },
    { "com.google.Gmail", "Gmail" },
    { "com.ubercab.UberClient", "Uber" },
    { "jp.co.jorudan.norikae", "乘换案内" },
    { "jp.co.navitime.transit", "NAVITIME" },
};

// 会推送逐步导航提示的地图 App。
static const char *const NAV_APPS[] = {
    "com.apple.Maps",
    "com.google.Maps",
    "com.autonavi.amapauto",
    "com.autonavi.minimap",
    "com.baidu.map",
};

void notif_list_init(notif_list_t *list) {
    memset(list, 0, sizeof(*list));
}

notif_kind_t notif_classify(const char *app_id, uint8_t category) {
    if (category == ANCS_CAT_INCOMING_CALL) return NOTIF_KIND_CALL;
    for (size_t i = 0; i < sizeof(NAV_APPS) / sizeof(NAV_APPS[0]); i++) {
        if (strcmp(app_id, NAV_APPS[i]) == 0) return NOTIF_KIND_NAV;
    }
    return NOTIF_KIND_MESSAGE;
}

static void copy_str(char *out, size_t cap, const char *src) {
    if (cap == 0) return;
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        // 退回到 UTF-8 字符边界
        while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, src, n);
    out[n] = '\0';
}

void notif_app_name(const char *app_id, char *out, size_t cap) {
    for (size_t i = 0; i < sizeof(APP_NAMES) / sizeof(APP_NAMES[0]); i++) {
        if (strcmp(app_id, APP_NAMES[i].bundle) == 0) {
            copy_str(out, cap, APP_NAMES[i].name);
            return;
        }
    }
    const char *dot = strrchr(app_id, '.');
    copy_str(out, cap, dot && dot[1] ? dot + 1 : (app_id[0] ? app_id : "通知"));
}

// 解码一个 UTF-8 字符。非法序列返回 0 并只前进 1 字节。
static uint32_t decode(const uint8_t *s, size_t *adv) {
    uint8_t c = s[0];
    if (c < 0x80) { *adv = 1; return c; }
    size_t n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    *adv = 1;
    if (n == 0) return 0;
    uint32_t cp = c & (0x7F >> n);
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    static const uint32_t MIN[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (cp < MIN[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    *adv = n;
    return cp;
}

static bool dropped(uint32_t cp) {
    return cp == 0
        || cp == '\r'
        || (cp >= 0x200B && cp <= 0x200F)     // 零宽字符、方向标记
        || (cp >= 0x2600 && cp <= 0x27BF)     // 杂项符号与装饰符号(多为表情)
        || (cp >= 0xE000 && cp <= 0xF8FF)     // 私用区
        || (cp >= 0xFE00 && cp <= 0xFE0F)     // 变体选择符
        || cp == 0x20E3                       // 组合键帽
        || cp >= 0x1F000;                     // 表情及其他补充平面符号
}

void notif_sanitize(char *text) {
    const uint8_t *in = (const uint8_t *)text;
    char *out = text;
    bool space = false;
    while (*in) {
        size_t adv;
        uint32_t cp = decode(in, &adv);
        bool is_space = cp == ' ' || cp == '\t' || cp == 0x3000;
        if (!dropped(cp) && !(is_space && space)) {
            if (cp == '\t') {
                *out++ = ' ';
            } else {
                memmove(out, in, adv);
                out += adv;
            }
        }
        if (!dropped(cp)) space = is_space;
        in += adv;
    }
    *out = '\0';
    // 去掉首尾空白
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\n')) text[--len] = '\0';
    size_t lead = 0;
    while (text[lead] == ' ' || text[lead] == '\n') lead++;
    if (lead) memmove(text, text + lead, len - lead + 1);
}

static ptrdiff_t find(const notif_list_t *list, uint32_t uid) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].uid == uid) return (ptrdiff_t)i;
    }
    return -1;
}

const notif_t *notif_list_upsert(notif_list_t *list, const ancs_attrs_t *attrs, uint8_t category,
                                 uint32_t now_ms) {
    notif_t n;
    memset(&n, 0, sizeof(n));
    n.uid = attrs->uid;
    n.seq = ++list->next_seq;
    n.at_ms = now_ms;
    n.kind = notif_classify(attrs->app_id, category);
    notif_app_name(attrs->app_id, n.app, sizeof(n.app));

    copy_str(n.title, sizeof(n.title), attrs->title);
    notif_sanitize(n.title);
    char subtitle[ANCS_SUBTITLE_SIZE];
    copy_str(subtitle, sizeof(subtitle), attrs->subtitle);
    notif_sanitize(subtitle);
    char message[ANCS_MESSAGE_SIZE];
    copy_str(message, sizeof(message), attrs->message);
    notif_sanitize(message);
    if (subtitle[0] && message[0]) {
        size_t sl = strlen(subtitle);
        memcpy(n.body, subtitle, sl);
        n.body[sl] = '\n';
        copy_str(n.body + sl + 1, sizeof(n.body) - sl - 1, message);
    } else {
        copy_str(n.body, sizeof(n.body), subtitle[0] ? subtitle : message);
    }
    if (!n.title[0]) copy_str(n.title, sizeof(n.title), n.app);

    ptrdiff_t at = find(list, n.uid);
    size_t shift_end;
    if (at >= 0) {
        shift_end = (size_t)at;               // 移除旧位置
    } else if (list->count < NOTIF_MAX) {
        shift_end = list->count++;
    } else {
        shift_end = NOTIF_MAX - 1;            // 丢掉最旧的
    }
    memmove(&list->items[1], &list->items[0], shift_end * sizeof(notif_t));
    list->items[0] = n;
    return &list->items[0];
}

bool notif_list_remove(notif_list_t *list, uint32_t uid) {
    ptrdiff_t at = find(list, uid);
    if (at < 0) return false;
    memmove(&list->items[at], &list->items[at + 1],
            (list->count - (size_t)at - 1) * sizeof(notif_t));
    list->count--;
    return true;
}

const notif_t *notif_list_latest_nav(const notif_list_t *list) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].kind == NOTIF_KIND_NAV) return &list->items[i];
    }
    return NULL;
}

void notif_fmt_age(uint32_t age_ms, char *out, size_t cap) {
    uint32_t min = age_ms / 60000u;
    if (min < 1) snprintf(out, cap, "刚刚");
    else if (min < 60) snprintf(out, cap, "%u 分钟前", (unsigned)min);
    else snprintf(out, cap, "%u 小时前", (unsigned)(min / 60));
}

static bool has(const char *text, const char *const *words) {
    for (; *words; words++) {
        if (strstr(text, *words)) return true;
    }
    return false;
}

notif_arrow_t notif_nav_arrow(const char *text) {
    // 掉头先判断:"掉头"里不含左右,但英文 "U-turn" 可能和 "left" 同句
    static const char *const UTURN[] = { "掉头", "调头", "Uターン", "U-turn", "U-Turn", "u-turn", NULL };
    static const char *const LEFT[] = { "左转", "左轉", "左折", "向左", "左前方", "left", "Left", NULL };
    static const char *const RIGHT[] = { "右转", "右轉", "右折", "向右", "右前方", "right", "Right", NULL };
    static const char *const STRAIGHT[] = { "直行", "直走", "直進", "继续", "まっすぐ", "straight",
                                            "Straight", "Continue", "continue", NULL };
    if (has(text, UTURN)) return NOTIF_ARROW_UTURN;
    bool left = has(text, LEFT);
    bool right = has(text, RIGHT);
    if (left != right) return left ? NOTIF_ARROW_LEFT : NOTIF_ARROW_RIGHT;
    if (!left && has(text, STRAIGHT)) return NOTIF_ARROW_STRAIGHT;
    return NOTIF_ARROW_NONE;
}
