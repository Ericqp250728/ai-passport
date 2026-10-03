// main/ancs_proto.h —— Apple Notification Center Service(ANCS)协议的纯逻辑部分。
// 不依赖 NimBLE/ESP-IDF,可在主机上测试。BLE 连接与 GATT 读写见 mirror_ble.c。
//
// ANCS 由 iPhone 充当 GATT 服务端,本设备是客户端:
//   Notification Source(通知)  8 字节事件:新增/修改/删除 + 分类 + 通知 UID
//   Control Point(写)         请求某个 UID 的属性(App、标题、正文)
//   Data Source(通知)         属性应答,可能被拆成多个 GATT 包,需要拼接
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    ANCS_EVENT_ADDED = 0,
    ANCS_EVENT_MODIFIED = 1,
    ANCS_EVENT_REMOVED = 2,
};

enum {
    ANCS_FLAG_SILENT = 1 << 0,
    ANCS_FLAG_IMPORTANT = 1 << 1,
    ANCS_FLAG_PRE_EXISTING = 1 << 2,
};

enum {
    ANCS_CAT_OTHER = 0,
    ANCS_CAT_INCOMING_CALL = 1,
    ANCS_CAT_MISSED_CALL = 2,
    ANCS_CAT_VOICEMAIL = 3,
    ANCS_CAT_SOCIAL = 4,
    ANCS_CAT_SCHEDULE = 5,
    ANCS_CAT_EMAIL = 6,
    ANCS_CAT_NEWS = 7,
    ANCS_CAT_HEALTH = 8,
    ANCS_CAT_BUSINESS = 9,
    ANCS_CAT_LOCATION = 10,
    ANCS_CAT_ENTERTAINMENT = 11,
};

// 各属性的缓冲大小(含结尾 0)。向 iPhone 请求的最大长度是"大小 - 1",
// iPhone 会在 UTF-8 字符边界之外截断时由本模块再修剪,保证结果总是合法 UTF-8。
#define ANCS_APP_ID_SIZE   48
#define ANCS_TITLE_SIZE    64
#define ANCS_SUBTITLE_SIZE 64
#define ANCS_MESSAGE_SIZE  192

typedef struct {
    uint8_t event;
    uint8_t flags;
    uint8_t category;
    uint8_t category_count;
    uint32_t uid;
} ancs_source_t;

typedef struct {
    uint32_t uid;
    char app_id[ANCS_APP_ID_SIZE];
    char title[ANCS_TITLE_SIZE];
    char subtitle[ANCS_SUBTITLE_SIZE];
    char message[ANCS_MESSAGE_SIZE];
} ancs_attrs_t;

// 解析 Notification Source 的 8 字节事件。长度不足返回 false。
bool ancs_parse_source(const uint8_t *data, size_t len, ancs_source_t *out);

// 生成 Control Point 的 GetNotificationAttributes 命令。返回字节数,cap 不足返回 0。
size_t ancs_build_get_attrs(uint32_t uid, uint8_t *buf, size_t cap);

// ---------------------------------------------------------------- Data Source 拼包

typedef enum {
    ANCS_RX_MORE = 0,   // 还没收完,等下一个包
    ANCS_RX_DONE,       // 收齐:*out 已填好
    ANCS_RX_ERROR,      // 格式不对或不是期待的 UID:丢弃本次应答
} ancs_rx_result_t;

typedef struct {
    uint32_t expect_uid;
    bool active;
    uint8_t header[5];
    uint8_t header_len;
    uint8_t attr_index;     // 已完成的属性个数
    uint8_t stage;          // 0=属性 ID,1=长度低字节,2=长度高字节,3=值
    uint8_t attr_id;
    uint16_t attr_len;
    uint16_t attr_pos;
    ancs_attrs_t attrs;
} ancs_rx_t;

// 发出请求后调用,开始接收该 UID 的应答。
void ancs_rx_begin(ancs_rx_t *rx, uint32_t uid);

// 喂入一个 Data Source 包。没有在接收时返回 ANCS_RX_ERROR。
ancs_rx_result_t ancs_rx_feed(ancs_rx_t *rx, const uint8_t *data, size_t len, ancs_attrs_t *out);

// ---------------------------------------------------------------- 待取属性的 UID 队列
//
// 一次只发一个属性请求,应答收齐后再发下一个;刚连上时 iPhone 会一次性推来
// 很多旧通知,队列满时丢弃最旧的,优先显示最新的。

#define ANCS_UIDQ_SIZE 8

typedef struct {
    uint32_t uid[ANCS_UIDQ_SIZE];
    uint8_t category[ANCS_UIDQ_SIZE];
    uint8_t head;
    uint8_t len;
} ancs_uidq_t;

void ancs_uidq_init(ancs_uidq_t *q);
// 已在队列中的 UID 不重复入队(更新其分类)。队列满时挤掉最旧的一条。
void ancs_uidq_push(ancs_uidq_t *q, uint32_t uid, uint8_t category);
bool ancs_uidq_pop(ancs_uidq_t *q, uint32_t *uid, uint8_t *category);
// 通知在取属性前就被删除时,从队列里撤掉。
void ancs_uidq_remove(ancs_uidq_t *q, uint32_t uid);
