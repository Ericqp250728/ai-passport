// main/mirror_ui.h —— 通知镜像界面。所有函数都必须在持有 bsp_lvgl_lock() 时调用。
#pragma once

#include <stdint.h>

#include "mirror_link.h"
#include "mirror_nav.h"
#include "notif_model.h"

// 创建屏幕、顶部状态栏和通知横幅。只调用一次。
void mirror_ui_init(const char *device_name);

// 整页重建(切页、打开/关闭详情、连接状态变化)。
void mirror_ui_show(const mirror_nav_t *nav, const notif_list_t *list, mirror_link_t link,
                    uint32_t now_ms);

// 局部刷新:消息选中项、详情翻页、清除配对提示、导航卡片的"几分钟前"。
void mirror_ui_refresh(const mirror_nav_t *nav, const notif_list_t *list, mirror_link_t link,
                       uint32_t now_ms);

// 详情页正文共有几页(打开详情并布局后有效),用于把翻页限制在范围内。
uint8_t mirror_ui_detail_pages(void);

// 顶部横幅提示新消息,约 5 秒后自动收起。
void mirror_ui_banner(const notif_t *n);

// 电量百分比;-1 表示读不到,此时隐藏。
void mirror_ui_set_battery(int soc);
