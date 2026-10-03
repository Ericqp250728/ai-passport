// main/badge_store.h —— 账本与设置的 NVS 持久化。只在 lifecycle 任务里调用(NVS 写入会阻塞数毫秒)。
#pragma once

#include "badge_ledger.h"
#include "esp_err.h"
#include <stdint.h>

// 初始化 NVS。分区损坏/版本不符时返回错误,而不是擦除整个 NVS(那会连带清掉其他数据)。
esp_err_t badge_store_init(void);

// 读取账本;不存在时得到空账本并返回 ESP_OK,数据损坏时得到空账本并返回 ESP_ERR_INVALID_CRC。
esp_err_t badge_store_load_ledger(badge_ledger_t *ledger);
esp_err_t badge_store_save_ledger(const badge_ledger_t *ledger);

// 背光档位。读取失败时返回 fallback。
uint8_t badge_store_load_brightness(uint8_t fallback);
esp_err_t badge_store_save_brightness(uint8_t level);
