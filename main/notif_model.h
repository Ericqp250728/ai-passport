// main/notif_model.h —— 镜像到设备上的通知列表:分类、App 名称、文本清洗。纯逻辑,可在主机上测试。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ancs_proto.h"

#define NOTIF_MAX         8
#define NOTIF_APP_SIZE    24
#define NOTIF_TITLE_SIZE  ANCS_TITLE_SIZE
#define NOTIF_BODY_SIZE   (ANCS_SUBTITLE_SIZE + ANCS_MESSAGE_SIZE)

typedef enum {
    NOTIF_KIND_MESSAGE = 0,
    NOTIF_KIND_NAV,       // 地图导航:单独的大字卡片
    NOTIF_KIND_CALL,      // 来电
} notif_kind_t;

typedef struct {
    uint32_t uid;
    uint32_t seq;         // 到达顺序,越大越新
    uint32_t at_ms;       // 最近一次收到/更新的时间(单调毫秒)
    notif_kind_t kind;
    char app[NOTIF_APP_SIZE];
    char title[NOTIF_TITLE_SIZE];
    char body[NOTIF_BODY_SIZE];
} notif_t;

typedef struct {
    notif_t items[NOTIF_MAX];   // 按新到旧排列
    size_t count;
    uint32_t next_seq;
} notif_list_t;

void notif_list_init(notif_list_t *list);

// 新增或更新(同一 UID 的"修改"事件,导航 App 常用它刷新下一步提示)。
// 更新后的条目移到最前。列表满时丢掉最旧的。返回存储位置。
const notif_t *notif_list_upsert(notif_list_t *list, const ancs_attrs_t *attrs, uint8_t category,
                                 uint32_t now_ms);

bool notif_list_remove(notif_list_t *list, uint32_t uid);

// 最新的一条导航通知;没有时返回 NULL。
const notif_t *notif_list_latest_nav(const notif_list_t *list);

// 来电优先,其次导航,最后普通消息。
notif_kind_t notif_classify(const char *app_id, uint8_t category);

// 常见 App 的中文名;未知 App 取包名最后一段。
void notif_app_name(const char *app_id, char *out, size_t cap);

// 就地清洗通知文本:去掉非法 UTF-8、表情符号、变体选择符、零宽字符和 \r,
// 连续空白合并为一个空格。字库不含表情,不清洗会显示成一串方框。
void notif_sanitize(char *text);

typedef enum {
    NOTIF_ARROW_NONE = 0,
    NOTIF_ARROW_LEFT,
    NOTIF_ARROW_RIGHT,
    NOTIF_ARROW_STRAIGHT,
    NOTIF_ARROW_UTURN,
} notif_arrow_t;

// 从导航提示文字猜转向(中文、日文、英文关键词)。认不出时返回 NOTIF_ARROW_NONE,
// 界面就不画箭头,而不是画一个可能错误的方向。
notif_arrow_t notif_nav_arrow(const char *text);

// 距今多久:"刚刚"、"3 分钟前"、"2 小时前"。
void notif_fmt_age(uint32_t age_ms, char *out, size_t cap);
