// main/mirror_link.h —— 与 iPhone 的连接状态。单独成文件,界面代码不必依赖 BLE/FreeRTOS 头文件。
#pragma once

typedef enum {
    MIRROR_LINK_OFF = 0,      // 蓝牙栈未运行(启动失败)
    MIRROR_LINK_ADVERTISING,  // 等待 iPhone 连接
    MIRROR_LINK_SECURING,     // 已连接,正在配对/加密
    MIRROR_LINK_DISCOVERING,  // 正在查找通知服务
    MIRROR_LINK_READY,        // 已订阅,通知会实时推送过来
    MIRROR_LINK_NO_ANCS,      // 连上了但没有通知服务:多半是没打开"共享系统通知"
} mirror_link_t;
