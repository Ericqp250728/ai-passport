// main/mirror_nav.h —— 通知镜像的页面、按键与亮屏状态机。纯逻辑,可在主机上测试。
//
// 三个页面循环:导航 → 消息 → 连接 → 导航
//
//   OK 短按      下一页(详情页中:返回消息列表)
//   OK 长按      消息页:打开选中消息的详情
//   上/下 短按   消息页:移动选中项        详情页:翻页
//   下 长按 ×2   连接页:3 秒内连续两次才清除配对,防误触
//
// 亮屏:按键后亮 20 秒,之后调暗,60 秒后熄屏;熄屏/调暗时的第一下按键只负责点亮。
// 新通知会点亮屏幕 15 秒;导航更新会自动切到导航页(正在看消息详情时除外)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MIRROR_PAGE_NAV = 0,
    MIRROR_PAGE_INBOX,
    MIRROR_PAGE_STATUS,
    MIRROR_PAGE_COUNT,
} mirror_page_t;

typedef enum {
    MIRROR_KEY_UP = 0,
    MIRROR_KEY_DOWN,
    MIRROR_KEY_OK,
} mirror_key_t;

typedef enum {
    MIRROR_PRESS_CLICK = 0,
    MIRROR_PRESS_LONG,
} mirror_press_t;

typedef enum {
    MIRROR_ACT_NONE = 0,
    MIRROR_ACT_PAGE,          // 页面(或详情开关)变化:整页重建
    MIRROR_ACT_SELECT,        // 消息选中项或详情翻页变化:局部刷新
    MIRROR_ACT_WAKE,          // 只点亮屏幕
    MIRROR_ACT_FORGET_ARM,    // 清除配对待确认
    MIRROR_ACT_FORGET_DISARM, // 待确认超时或被取消
    MIRROR_ACT_FORGET,        // 清除配对
    MIRROR_ACT_BACKLIGHT,     // 只有背光档位变化
} mirror_act_t;

#define MIRROR_AWAKE_MS        20000u
#define MIRROR_DIM_MS          60000u
#define MIRROR_NOTIF_WAKE_MS   15000u
#define MIRROR_CONFIRM_MS       3000u
#define MIRROR_BRIGHT_PERCENT     80
#define MIRROR_DIM_PERCENT        12

typedef struct {
    mirror_page_t page;
    bool detail;              // 消息详情是否打开
    size_t selected;          // 消息页选中项(0 = 最新)
    size_t count;             // 当前消息条数
    uint8_t detail_page;      // 详情翻页
    bool forget_armed;
    uint32_t forget_deadline_ms;
    uint32_t last_input_ms;
    uint32_t wake_until_ms;
    uint8_t backlight;        // 最近一次计算出的背光百分比
} mirror_nav_t;

void mirror_nav_init(mirror_nav_t *nav, uint32_t now_ms);

mirror_act_t mirror_nav_input(mirror_nav_t *nav, mirror_key_t key, mirror_press_t press,
                              uint32_t now_ms);

// 消息条数变化(新增/删除)后调用;选中项保持在合法范围,详情的那条被删时关闭详情。
// selected_still_exists=false 表示详情里正在看的那条消息已被删除。
mirror_act_t mirror_nav_set_count(mirror_nav_t *nav, size_t count, bool selected_still_exists);

// 收到通知。is_nav=true 时切到导航页(除非正在看详情)。总会点亮屏幕一段时间。
mirror_act_t mirror_nav_on_notif(mirror_nav_t *nav, bool is_nav, uint32_t now_ms);

// 周期调用(约每 0.5 秒)。返回超时动作:清除配对确认超时、背光档位变化。
mirror_act_t mirror_nav_tick(mirror_nav_t *nav, uint32_t now_ms);

// 当前应使用的背光百分比(0 = 熄屏)。
uint8_t mirror_nav_backlight(const mirror_nav_t *nav, uint32_t now_ms);
