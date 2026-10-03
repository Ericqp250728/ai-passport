// main/mirror_ble.c —— NimBLE 外设 + ANCS GATT 客户端。
//
// 连接后的流程严格串行(ATT 同一时刻只能有一个未完成的请求):
//   加密 → 交换 MTU → 发现 ANCS 服务 → 发现特征 → 订阅 Data Source → 订阅 Notification Source → READY
// 之后每条通知:Notification Source 事件 → UID 入队 → 写 Control Point 取属性
//               → Data Source 分包拼接 → 投递给应用任务 → 取下一条。
#include "mirror_ble.h"

#include "esp_log.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>

static const char *TAG = "mirror_ble";

// 所有 UUID 按小端字节序书写(NimBLE 的 128 位 UUID 存储格式)。
// 服务 7905F431-B5CE-4E99-A40F-4B1E122D00D0
static const ble_uuid128_t UUID_ANCS = BLE_UUID128_INIT(
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, 0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79);
// Notification Source 9FBF120D-6301-42D9-8C58-25E699A21DBD
static const ble_uuid128_t UUID_NOTIF_SRC = BLE_UUID128_INIT(
    0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C, 0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F);
// Control Point 69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9
static const ble_uuid128_t UUID_CTRL_PT = BLE_UUID128_INIT(
    0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98, 0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69);
// Data Source 22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB
static const ble_uuid128_t UUID_DATA_SRC = BLE_UUID128_INIT(
    0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE, 0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22);

#define APPEARANCE_GENERIC_WATCH 0x00C0
#define REQUEST_TIMEOUT_MS       3000
#define MAX_CHRS                 6

typedef struct {
    uint16_t def;
    uint16_t val;
} chr_handles_t;

static QueueHandle_t s_events;
static const char *s_name;
static uint8_t s_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;

// 以下状态只在 host 任务里读写
static uint16_t s_svc_start, s_svc_end;
static chr_handles_t s_chrs[MAX_CHRS];
static size_t s_chr_count;
static uint16_t s_notif_src, s_ctrl_pt, s_data_src;
static uint16_t s_cccd;              // 当前正在订阅的特征的 CCCD
static bool s_subscribing_data;      // true=正在订阅 Data Source,false=Notification Source
static bool s_ready;

static ancs_uidq_t s_queue;
static ancs_rx_t s_rx;
static uint8_t s_req_category;
static bool s_req_busy;
static struct ble_npl_callout s_req_timer;
static struct ble_npl_event s_forget_ev;
static mirror_event_t s_ev;          // 投递用的暂存区:结构体较大,不放在 host 任务栈上
static uint8_t s_pkt[512];

static int gap_event(struct ble_gap_event *event, void *arg);
static void pump(void);

static void post_link(mirror_link_t link) {
    memset(&s_ev, 0, sizeof(s_ev));
    s_ev.type = MIRROR_EV_LINK;
    s_ev.link = link;
    if (xQueueSend(s_events, &s_ev, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "状态事件队列已满");
    }
}

static void reset_session(void) {
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_svc_start = s_svc_end = 0;
    s_chr_count = 0;
    s_notif_src = s_ctrl_pt = s_data_src = 0;
    s_cccd = 0;
    s_ready = false;
    s_req_busy = false;
    s_rx.active = false;
    ancs_uidq_init(&s_queue);
    ble_npl_callout_stop(&s_req_timer);
}

static void fail_link(const char *why, int rc) {
    ESP_LOGW(TAG, "%s (rc=%d),断开", why, rc);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

// ---------------------------------------------------------------- 广播

static void advertise(void) {
    // 广播包:标志 + 征求 ANCS 服务。征求 ANCS 是 iPhone 在 设置 → 蓝牙 中列出本设备的前提。
    struct ble_hs_adv_fields adv = { 0 };
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.sol_uuids128 = &UUID_ANCS;
    adv.sol_num_uuids128 = 1;
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "广播数据设置失败 rc=%d", rc);
        return;
    }

    // 扫描应答:设备名 + 外观(手表),iPhone 列表里显示名字
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = (uint8_t)strlen(s_name);
    rsp.name_is_complete = 1;
    rsp.appearance = APPEARANCE_GENERIC_WATCH;
    rsp.appearance_is_present = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "扫描应答设置失败 rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // Apple 配件设计指南推荐的省电间隔 152.5–211.25 ms(单位 0.625 ms):iPhone 仍能很快发现,
    // 待机耗电比默认的 30–60 ms 低得多
    params.itvl_min = 244;
    params.itvl_max = 338;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "开始广播失败 rc=%d", rc);
        return;
    }
    post_link(MIRROR_LINK_ADVERTISING);
}

// ---------------------------------------------------------------- 订阅

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg);
static void subscribe(uint16_t val_handle);

static int on_dsc(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val,
                  const struct ble_gatt_dsc *dsc, void *arg) {
    (void)chr_val;
    (void)arg;
    if (error->status == 0) {
        if (!s_cccd && ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
            s_cccd = dsc->handle;
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        fail_link("描述符发现失败", error->status);
        return 0;
    }
    // EDONE:发现结束。只有在这里、确认找到 CCCD 后才写订阅。
    if (!s_cccd) {
        fail_link("通知特征没有 CCCD", 0);
        return 0;
    }
    static const uint8_t enable[2] = { 0x01, 0x00 };
    int rc = ble_gattc_write_flat(conn, s_cccd, enable, sizeof(enable), on_cccd_written, NULL);
    if (rc != 0) fail_link("写 CCCD 失败", rc);
    return 0;
}

static uint16_t chr_end(uint16_t val_handle) {
    uint16_t end = s_svc_end;
    for (size_t i = 0; i < s_chr_count; i++) {
        if (s_chrs[i].def > val_handle && s_chrs[i].def - 1 < end) end = s_chrs[i].def - 1;
    }
    return end;
}

static void subscribe(uint16_t val_handle) {
    s_cccd = 0;
    uint16_t end = chr_end(val_handle);
    if (end <= val_handle) {
        fail_link("特征后面没有描述符", 0);
        return;
    }
    int rc = ble_gattc_disc_all_dscs(s_conn, val_handle, end, on_dsc, NULL);
    if (rc != 0) fail_link("开始描述符发现失败", rc);
}

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg) {
    (void)conn;
    (void)attr;
    (void)arg;
    if (error->status != 0) {
        // 未加密时 iPhone 会拒绝(0x05/0x0F);正常流程不会走到这里
        fail_link("订阅被拒绝", error->status);
        return 0;
    }
    if (s_subscribing_data) {
        // Apple 建议先订阅 Data Source,再订阅 Notification Source,免得漏掉属性应答
        s_subscribing_data = false;
        subscribe(s_notif_src);
        return 0;
    }
    s_ready = true;
    ESP_LOGI(TAG, "ANCS 已订阅,开始接收通知");
    post_link(MIRROR_LINK_READY);
    pump();
    return 0;
}

// ---------------------------------------------------------------- 发现

static int on_chr(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg) {
    (void)conn;
    (void)arg;
    if (error->status == 0) {
        if (s_chr_count < MAX_CHRS) {
            s_chrs[s_chr_count++] = (chr_handles_t){ .def = chr->def_handle, .val = chr->val_handle };
        }
        if (ble_uuid_cmp(&chr->uuid.u, &UUID_NOTIF_SRC.u) == 0) s_notif_src = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &UUID_CTRL_PT.u) == 0) s_ctrl_pt = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &UUID_DATA_SRC.u) == 0) s_data_src = chr->val_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        fail_link("特征发现失败", error->status);
        return 0;
    }
    if (!s_notif_src || !s_ctrl_pt || !s_data_src) {
        fail_link("ANCS 特征不完整", 0);
        return 0;
    }
    s_subscribing_data = true;
    subscribe(s_data_src);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *svc, void *arg) {
    (void)arg;
    if (error->status == 0) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        fail_link("服务发现失败", error->status);
        return 0;
    }
    if (!s_svc_start) {
        // 连着但看不到 ANCS:iPhone 上没有允许"共享系统通知"。保持连接,等用户打开后会重连。
        ESP_LOGW(TAG, "iPhone 未提供 ANCS 服务");
        post_link(MIRROR_LINK_NO_ANCS);
        return 0;
    }
    int rc = ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, on_chr, NULL);
    if (rc != 0) fail_link("开始特征发现失败", rc);
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *error, uint16_t mtu, void *arg) {
    (void)arg;
    // MTU 协商失败不影响功能(iPhone 可能已主动协商过),只是包更小
    if (error->status == 0) ESP_LOGI(TAG, "MTU=%u", mtu);
    int rc = ble_gattc_disc_svc_by_uuid(conn, &UUID_ANCS.u, on_svc, NULL);
    if (rc != 0) fail_link("开始服务发现失败", rc);
    return 0;
}

// ---------------------------------------------------------------- 取通知属性

static void finish_request(void) {
    s_req_busy = false;
    s_rx.active = false;
    ble_npl_callout_stop(&s_req_timer);
    pump();
}

static int on_ctrl_written(uint16_t conn, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg) {
    (void)conn;
    (void)attr;
    (void)arg;
    if (error->status != 0) {
        // 常见:通知在请求发出前已被删除(ANCS 错误 0xA2),跳过即可
        ESP_LOGD(TAG, "取属性被拒 status=0x%x", error->status);
        finish_request();
    }
    return 0;
}

static void pump(void) {
    if (!s_ready || s_req_busy) return;
    uint32_t uid;
    uint8_t category;
    if (!ancs_uidq_pop(&s_queue, &uid, &category)) return;

    uint8_t cmd[32];
    size_t len = ancs_build_get_attrs(uid, cmd, sizeof(cmd));
    ancs_rx_begin(&s_rx, uid);
    s_req_category = category;
    s_req_busy = true;
    int rc = ble_gattc_write_flat(s_conn, s_ctrl_pt, cmd, (uint16_t)len, on_ctrl_written, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "写 Control Point 失败 rc=%d", rc);
        s_req_busy = false;
        s_rx.active = false;
        return;
    }
    ble_npl_callout_reset(&s_req_timer, ble_npl_time_ms_to_ticks32(REQUEST_TIMEOUT_MS));
}

static void on_request_timeout(struct ble_npl_event *ev) {
    (void)ev;
    if (!s_req_busy) return;
    ESP_LOGW(TAG, "通知属性应答超时,跳过");
    finish_request();
}

static void on_notif_source(const uint8_t *data, size_t len) {
    ancs_source_t src;
    if (!ancs_parse_source(data, len, &src)) return;
    if (src.event == ANCS_EVENT_REMOVED) {
        ancs_uidq_remove(&s_queue, src.uid);
        memset(&s_ev, 0, sizeof(s_ev));
        s_ev.type = MIRROR_EV_REMOVED;
        s_ev.uid = src.uid;
        (void)xQueueSend(s_events, &s_ev, 0);
        return;
    }
    // 队列里的分类字节最高位借来标记"旧通知"
    uint8_t tag = (uint8_t)(src.category & 0x7F);
    if (src.flags & ANCS_FLAG_PRE_EXISTING) tag |= 0x80;
    ancs_uidq_push(&s_queue, src.uid, tag);
    pump();
}

static void on_data_source(const uint8_t *data, size_t len) {
    if (!s_req_busy) return;
    ancs_rx_result_t r = ancs_rx_feed(&s_rx, data, len, &s_ev.attrs);
    if (r == ANCS_RX_MORE) return;
    if (r == ANCS_RX_DONE) {
        s_ev.type = MIRROR_EV_NOTIF;
        s_ev.uid = s_ev.attrs.uid;
        s_ev.category = s_req_category & 0x7F;
        s_ev.pre_existing = (s_req_category & 0x80) != 0;
        // 刚连上时旧通知会集中到达,稍等应用任务取走,尽量不丢
        if (xQueueSend(s_events, &s_ev, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "通知队列已满,丢弃一条");
        }
    } else {
        ESP_LOGW(TAG, "通知属性应答格式错误,跳过");
    }
    finish_request();
}

// ---------------------------------------------------------------- GAP

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            return 0;
        }
        reset_session();
        s_conn = event->connect.conn_handle;
        ESP_LOGI(TAG, "iPhone 已连接,开始加密");
        post_link(MIRROR_LINK_SECURING);
        // 已绑定时直接用保存的密钥加密;未绑定时触发配对,iPhone 会弹出配对请求
        if (ble_gap_security_initiate(s_conn) != 0) fail_link("无法发起加密", 0);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "断开 reason=0x%x", event->disconnect.reason);
        reset_session();
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status != 0) {
            // 多半是 iPhone 那边已"忽略此设备",旧密钥失效:删掉本地绑定,下次重新配对
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            fail_link("加密失败,已删除旧配对", event->enc_change.status);
            return 0;
        }
        post_link(MIRROR_LINK_DISCOVERING);
        if (ble_gattc_exchange_mtu(event->enc_change.conn_handle, on_mtu, NULL) != 0) {
            on_mtu(event->enc_change.conn_handle, &(struct ble_gatt_error){ .status = BLE_HS_EALREADY },
                   0, NULL);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // iPhone 重新配对(比如它删掉过本设备):丢弃旧绑定,接受新配对
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len > sizeof(s_pkt)) len = sizeof(s_pkt);
        if (os_mbuf_copydata(event->notify_rx.om, 0, len, s_pkt) != 0) return 0;
        if (event->notify_rx.attr_handle == s_notif_src) on_notif_source(s_pkt, len);
        else if (event->notify_rx.attr_handle == s_data_src) on_data_source(s_pkt, len);
        return 0;
    }

    default:
        return 0;
    }
}

// ---------------------------------------------------------------- host

static void on_forget(struct ble_npl_event *ev) {
    (void)ev;
    int rc = ble_store_clear();
    ESP_LOGI(TAG, "已清除全部配对 rc=%d", rc);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE 地址初始化失败 rc=%d", rc);
        post_link(MIRROR_LINK_OFF);
        return;
    }
    advertise();
}

static void on_reset(int reason) {
    ESP_LOGE(TAG, "BLE host 复位 reason=%d", reason);
    reset_session();
}

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();   // 本应用不停止蓝牙,正常情况下不会返回
    vTaskDelete(NULL);
}

void ble_store_config_init(void);

esp_err_t mirror_ble_start(QueueHandle_t events, const char *device_name) {
    s_events = events;
    s_name = device_name;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // 无输入输出能力:"Just Works" 配对,iPhone 只需点"配对"。
    // 交换 ID 密钥,iPhone 换随机地址后也能认出已绑定的它。
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_svc_gap_device_name_set(device_name) != 0
        || ble_svc_gap_device_appearance_set(APPEARANCE_GENERIC_WATCH) != 0) {
        return ESP_FAIL;
    }
    ble_store_config_init();   // 绑定信息存 NVS(CONFIG_BT_NIMBLE_NVS_PERSIST)

    ancs_uidq_init(&s_queue);
    ble_npl_callout_init(&s_req_timer, nimble_port_get_dflt_eventq(), on_request_timeout, NULL);
    ble_npl_event_init(&s_forget_ev, on_forget, NULL);

    if (xTaskCreate(host_task, "nimble_host", 4608, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool mirror_ble_has_bond(void) {
    int count = 0;
    return ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &count) == 0 && count > 0;
}

void mirror_ble_forget(void) {
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_forget_ev);
}
