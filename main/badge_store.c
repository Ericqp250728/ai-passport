// main/badge_store.c —— NVS 命名空间 "badge":ledger(blob)与 bright(u8)。
#include "badge_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "badge_store";
static const char *NS = "badge";
static bool s_ready;

esp_err_t badge_store_init(void) {
    if (s_ready) return ESP_OK;
    esp_err_t e = nvs_flash_init();
    if (e != ESP_OK) {
        // 不自动 nvs_flash_erase():那会掩盖分区问题并清掉别的数据。账本只在本次运行中保存在 RAM。
        ESP_LOGE(TAG, "NVS 初始化失败(%s),账本将不会掉电保存", esp_err_to_name(e));
        return e;
    }
    s_ready = true;
    return ESP_OK;
}

esp_err_t badge_store_load_ledger(badge_ledger_t *ledger) {
    badge_ledger_reset(ledger);
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;    // 首次开机,命名空间还不存在
    if (e != ESP_OK) return e;

    uint8_t blob[BADGE_LEDGER_BLOB_SIZE];
    size_t len = sizeof(blob);
    e = nvs_get_blob(h, "ledger", blob, &len);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) return e;
    if (!badge_ledger_deserialize(ledger, blob, len)) {
        ESP_LOGW(TAG, "账本数据损坏或版本不符,已从空账本开始");
        badge_ledger_reset(ledger);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t badge_store_save_ledger(const badge_ledger_t *ledger) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    uint8_t blob[BADGE_LEDGER_BLOB_SIZE];
    size_t len = badge_ledger_serialize(ledger, blob, sizeof(blob));
    if (len == 0) return ESP_ERR_INVALID_SIZE;

    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "ledger", blob, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

uint8_t badge_store_load_brightness(uint8_t fallback) {
    if (!s_ready) return fallback;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return fallback;
    uint8_t level = fallback;
    if (nvs_get_u8(h, "bright", &level) != ESP_OK) level = fallback;
    nvs_close(h);
    return level;
}

esp_err_t badge_store_save_brightness(uint8_t level) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, "bright", level);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
