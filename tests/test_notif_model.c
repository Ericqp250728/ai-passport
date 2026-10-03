#include <assert.h>
#include <string.h>

#include "notif_model.h"

static ancs_attrs_t attrs(uint32_t uid, const char *app, const char *title,
                          const char *sub, const char *msg) {
    ancs_attrs_t a;
    memset(&a, 0, sizeof(a));
    a.uid = uid;
    strcpy(a.app_id, app);
    strcpy(a.title, title);
    strcpy(a.subtitle, sub);
    strcpy(a.message, msg);
    return a;
}

static void sanitized(const char *in, const char *want) {
    char buf[128];
    strcpy(buf, in);
    notif_sanitize(buf);
    assert(strcmp(buf, want) == 0);
}

static void test_sanitize(void) {
    sanitized("你好\xF0\x9F\x98\x80!", "你好!");                     // 😀
    sanitized("\xE2\x9D\xA4\xEF\xB8\x8F 爱心", "爱心");               // ❤️ + 变体选择符
    sanitized("a\r\nb", "a\nb");
    sanitized("  前  后\t\t空格  ", "前 后 空格");
    sanitized("坏\xFF字节", "坏字节");
    sanitized("\xE9\xA7", "");                                       // 残缺多字节
    sanitized("\xC0\xAF", "");                                       // 过长编码
    sanitized("新宿駅 → 3 番線", "新宿駅 → 3 番線");                  // 箭头和日文保留
}

static void test_classify_and_names(void) {
    assert(notif_classify("com.google.Maps", ANCS_CAT_LOCATION) == NOTIF_KIND_NAV);
    assert(notif_classify("com.apple.Maps", ANCS_CAT_OTHER) == NOTIF_KIND_NAV);
    assert(notif_classify("com.tencent.xin", ANCS_CAT_SOCIAL) == NOTIF_KIND_MESSAGE);
    assert(notif_classify("com.apple.mobilephone", ANCS_CAT_INCOMING_CALL) == NOTIF_KIND_CALL);

    char name[NOTIF_APP_SIZE];
    notif_app_name("com.tencent.xin", name, sizeof(name));
    assert(strcmp(name, "微信") == 0);
    notif_app_name("com.example.SomeApp", name, sizeof(name));
    assert(strcmp(name, "SomeApp") == 0);
    notif_app_name("", name, sizeof(name));
    assert(strcmp(name, "通知") == 0);
    char tiny[4];
    notif_app_name("com.tencent.xin", tiny, sizeof(tiny));          // "微信" 放不下:按字符截断
    assert(strcmp(tiny, "微") == 0);
}

static void test_list_order_and_update(void) {
    notif_list_t l;
    notif_list_init(&l);
    ancs_attrs_t a = attrs(1, "com.tencent.xin", "小林", "", "在吗");
    ancs_attrs_t b = attrs(2, "com.google.Maps", "300 m", "", "右转进入 明治通り");
    notif_list_upsert(&l, &a, ANCS_CAT_SOCIAL, 0);
    notif_list_upsert(&l, &b, ANCS_CAT_LOCATION, 0);
    assert(l.count == 2 && l.items[0].uid == 2);
    assert(strcmp(l.items[1].app, "微信") == 0);
    assert(notif_list_latest_nav(&l)->uid == 2);

    // 导航更新同一 UID:替换内容,不新增条目
    ancs_attrs_t b2 = attrs(2, "com.google.Maps", "50 m", "", "右转");
    const notif_t *n = notif_list_upsert(&l, &b2, ANCS_CAT_LOCATION, 0);
    assert(l.count == 2 && strcmp(n->title, "50 m") == 0);
    // 旧消息再次更新:移到最前
    notif_list_upsert(&l, &a, ANCS_CAT_SOCIAL, 0);
    assert(l.items[0].uid == 1 && l.items[1].uid == 2);
    assert(l.items[0].seq > l.items[1].seq);

    assert(notif_list_remove(&l, 2));
    assert(!notif_list_remove(&l, 2));
    assert(l.count == 1 && notif_list_latest_nav(&l) == NULL);
}

static void test_list_capacity_and_body(void) {
    notif_list_t l;
    notif_list_init(&l);
    for (uint32_t i = 0; i < NOTIF_MAX + 3; i++) {
        ancs_attrs_t a = attrs(100 + i, "jp.naver.line", "", "", "hi");
        notif_list_upsert(&l, &a, ANCS_CAT_SOCIAL, 0);
    }
    assert(l.count == NOTIF_MAX);
    assert(l.items[0].uid == 100 + NOTIF_MAX + 2);
    assert(l.items[NOTIF_MAX - 1].uid == 103);
    assert(strcmp(l.items[0].title, "LINE") == 0);   // 没标题时用 App 名

    ancs_attrs_t s = attrs(1, "com.apple.MobileSMS", "10086", "账单", "本月话费");
    const notif_t *n = notif_list_upsert(&l, &s, ANCS_CAT_SOCIAL, 0);
    assert(strcmp(n->body, "账单\n本月话费") == 0);
}

static void test_age(void) {
    char buf[32];
    notif_fmt_age(59999, buf, sizeof(buf));
    assert(strcmp(buf, "刚刚") == 0);
    notif_fmt_age(3 * 60000, buf, sizeof(buf));
    assert(strcmp(buf, "3 分钟前") == 0);
    notif_fmt_age(125 * 60000u, buf, sizeof(buf));
    assert(strcmp(buf, "2 小时前") == 0);
}

static void test_nav_arrow(void) {
    assert(notif_nav_arrow("300 m 后右转") == NOTIF_ARROW_RIGHT);
    assert(notif_nav_arrow("次の交差点を左折") == NOTIF_ARROW_LEFT);
    assert(notif_nav_arrow("Turn right onto Meiji-dori") == NOTIF_ARROW_RIGHT);
    assert(notif_nav_arrow("直行 500 米") == NOTIF_ARROW_STRAIGHT);
    assert(notif_nav_arrow("前方掉头") == NOTIF_ARROW_UTURN);
    assert(notif_nav_arrow("Make a U-turn, then turn left") == NOTIF_ARROW_UTURN);
    assert(notif_nav_arrow("左转后右转") == NOTIF_ARROW_NONE);      // 同时出现:不猜
    assert(notif_nav_arrow("到达 新宿駅") == NOTIF_ARROW_NONE);
}

int main(void) {
    test_nav_arrow();
    test_age();
    test_sanitize();
    test_classify_and_names();
    test_list_order_and_update();
    test_list_capacity_and_body();
    return 0;
}
