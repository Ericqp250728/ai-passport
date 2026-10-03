// main/mirror_ble.h —— 通过 BLE ANCS 读取 iPhone 通知。
//
// 设备作为外设广播并"征求"ANCS 服务,因此会出现在 iPhone 的 设置 → 蓝牙 列表里。
// iPhone 连接并配对(绑定信息存 NVS,下次自动重连)后,本模块按顺序发现 ANCS、
// 订阅 Data Source 与 Notification Source,逐条取回通知属性,通过队列交给应用任务。
//
// 所有 NimBLE 回调都在 BLE host 任务里运行;本模块不接触 LVGL。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ancs_proto.h"
#include "mirror_link.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    MIRROR_EV_LINK = 0,       // link 字段有效
    MIRROR_EV_NOTIF,          // attrs + category 有效(新增或更新)
    MIRROR_EV_REMOVED,        // uid 有效
} mirror_ev_type_t;

typedef struct {
    mirror_ev_type_t type;
    mirror_link_t link;
    uint8_t category;
    bool pre_existing;        // 连接时 iPhone 补发的旧通知:静默加入,不弹横幅不亮屏
    uint32_t uid;
    ancs_attrs_t attrs;
} mirror_event_t;

// 启动 BLE。调用前必须已 nvs_flash_init()(绑定信息与射频校准数据都存在 NVS)。
// events 是 mirror_event_t 的队列;队列满时新的通知会被丢弃,状态变化会短暂等待。
esp_err_t mirror_ble_start(QueueHandle_t events, const char *device_name);

// 是否已与某台 iPhone 配对过(mirror_ble_start 之后调用)。
bool mirror_ble_has_bond(void);

// 清除所有绑定并断开连接,之后需要在 iPhone 上"忽略此设备"再重新配对。非阻塞。
void mirror_ble_forget(void);
