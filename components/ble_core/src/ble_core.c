/*
 * BLE glue, see ble_core.h. The NimBLE host task owns every NimBLE state change (advertising, connection
 * parameters, pairing); the app task talks to it through two FIFOs (events up, notifications down), a few
 * volatile flags and the read values behind one spinlock.
 */
#include "epager/ble_core.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "host/ble_hs.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "sdkconfig.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "epager/ancs_glue.h"
#include "epager/ble_util.h"
#include "epager/proto_gen.h"

#define TAG "ble"

/* NimBLE's NVS bond store; not declared in a public header (the ESP-IDF ble_ancs example does the same). */
void ble_store_config_init(void);

/* Characteristics, numbered as gatt.json's char_id (1-5 are core's CORE_CHR_*) and our own for the rest. */
enum { CHR_CAPABILITIES = 1, CHR_CONFIG, CHR_NOTIFICATION, CHR_TRANSFER, CHR_STATUS, CHR_BATTERY, CHR_DIS_MANUF,
       CHR_DIS_MODEL, CHR_DIS_FW, CHR_COUNT };

#define RX_FIFO_BYTES (16u * 1024u) /* a transfer window (4 KiB) and the ANCS replay with room to spare */
#define TX_FIFO_BYTES (16u * 1024u)
#define READ_MAX 512u
#define TICK_MS 250u
static volatile uint32_t tick_ms = TICK_MS; /* A/B drain tests only (D106, D110) */
#define PUMP_BATCH 8
#define MAX_PEERS 8

static struct {
    blec_config_t cfg;
    char name[BLEU_NAME_MAX];
    bleu_fifo_t rx, tx; /* rx: host task -> app task (guarded by lock); tx: app task only */
    portMUX_TYPE lock;
    uint8_t rd[2][READ_MAX]; /* Capabilities, Status */
    uint16_t rd_len[2];
    uint8_t own_addr_type;
    bool synced, enabled, started; /* host synced; blec_start() called; advertising begun */
    volatile bool running;  /* NimBLE is up (blec_init / blec_resume until blec_stop) */
    volatile bool halting;  /* blec_stop under way: no advertising, no tick */
    uint8_t bonds_cached;   /* blec_bond_count while stopped */
    uint16_t conn;                          /* host task */
    volatile uint16_t conn_pub;             /* for the app task's notifications */
    volatile bool connected, secure, pairing_mode, busy, reapply, sub[CHR_COUNT];
    volatile uint8_t battery;
    bool pairing_active;                    /* a passkey was shown on this connection */
    bool peer_was_bonded;                   /* identity matched the store when this connection arrived */
    uint32_t connect_ms, adv_start_ms;
    uint16_t adv_units;
    bool adv_on, adv_failed, dropping; /* dropping: terminate already asked */
    bleu_conn_policy_t policy;
    uint16_t val_h[CHR_COUNT];
    struct ble_npl_callout tick;
    struct ble_npl_event ev_forget, ev_forget_one, ev_start;
    volatile size_t forget_index;           /* blec_forget_bond: which bond ev_forget_one drops */
    uint32_t rx_bytes, tx_bytes, stat_ms;   /* EPAGER_BLE_STATS */
} g = {.lock = portMUX_INITIALIZER_UNLOCKED, .conn = BLE_HS_CONN_HANDLE_NONE, .conn_pub = BLE_HS_CONN_HANDLE_NONE,
       .battery = 255};

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------------------------------------------------------------- events up to the app task */

/* Data (writes, ANCS values) may fill the FIFO only up to its cap minus a reserve: connection, security and
 * pairing events must always get through, or the app task's idea of the link goes stale. */
#define RX_RESERVE 2048u

static bool post(blec_event_kind_t kind, const uint8_t *data, size_t len)
{
    const bool is_data = kind == BLEC_EV_WRITE || kind == BLEC_EV_ANCS_NS || kind == BLEC_EV_ANCS_DS;
    portENTER_CRITICAL(&g.lock);
    const bool ok = (!is_data || g.rx.cap - g.rx.used > RX_RESERVE + BLEU_FIFO_HDR + len) &&
                    bleu_fifo_push(&g.rx, (uint8_t)kind, data, (uint16_t)len);
    portEXIT_CRITICAL(&g.lock);
    if (ok && g.cfg.wake) {
        g.cfg.wake(g.cfg.ctx);
    } else if (!ok) {
        ESP_LOGW(TAG, "event queue full: dropped (%u so far)", (unsigned)g.rx.drops);
    }
    return ok;
}

static void post_u16(blec_event_kind_t kind, uint16_t v)
{
    const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    post(kind, b, sizeof(b));
}

static void post_u8(blec_event_kind_t kind, uint8_t v)
{
    post(kind, &v, 1);
}

static void ancs_post(ancsg_event_t ev, const uint8_t *data, size_t len)
{
    switch (ev) {
    case ANCSG_EV_READY:
        post(BLEC_EV_ANCS_READY, NULL, 0);
        break;
    case ANCSG_EV_LOST:
        post(BLEC_EV_ANCS_LOST, NULL, 0);
        break;
    case ANCSG_EV_NS:
        post(BLEC_EV_ANCS_NS, data, len);
        break;
    case ANCSG_EV_DS:
        post(BLEC_EV_ANCS_DS, data, len);
        break;
    case ANCSG_EV_WRITE:
        post(BLEC_EV_ANCS_WRITE, data, len);
        break;
    }
}

bool blec_poll(blec_event_t *ev)
{
    static uint8_t rec[1u + BLEC_DATA_MAX];
    uint8_t tag = 0;
    uint16_t len = 0;
    portENTER_CRITICAL(&g.lock);
    const bool have = bleu_fifo_peek(&g.rx, &tag, rec, sizeof(rec), &len);
    if (have) {
        bleu_fifo_drop(&g.rx);
    }
    portEXIT_CRITICAL(&g.lock);
    if (!have) {
        return false;
    }
    ev->kind = (blec_event_kind_t)tag;
    ev->chr = 0;
    ev->len = ev->mtu = 0;
    ev->passkey = 0;
    ev->ok = ev->bonds = 0;
    switch (ev->kind) {
    case BLEC_EV_CONNECTED:
    case BLEC_EV_DISCONNECTED: /* mtu = the reason (NimBLE's code: 0x200 + the HCI reason), 0 if there was none */
    case BLEC_EV_MTU:
        ev->mtu = len >= 2 ? (uint16_t)(rec[0] | rec[1] << 8) : 0;
        break;
    case BLEC_EV_PASSKEY:
        ev->passkey = len >= 4 ? (uint32_t)rec[0] | (uint32_t)rec[1] << 8 | (uint32_t)rec[2] << 16 | (uint32_t)rec[3] << 24 : 0;
        break;
    case BLEC_EV_PAIR_DONE:
    case BLEC_EV_ANCS_WRITE:
        ev->ok = len >= 1 ? rec[0] : 0;
        break;
    case BLEC_EV_BONDS:
        ev->bonds = len >= 1 ? rec[0] : 0;
        break;
    case BLEC_EV_WRITE:
        if (len >= 1) {
            ev->chr = rec[0];
            ev->len = (uint16_t)(len - 1);
            memcpy(ev->data, rec + 1, ev->len);
        }
        break;
    case BLEC_EV_ANCS_NS:
    case BLEC_EV_ANCS_DS:
        ev->len = len;
        memcpy(ev->data, rec, len);
        break;
    default:
        break;
    }
    return true;
}

/* ---------------------------------------------------------------- notifications down to the phone */

void blec_notify(uint8_t chr, const uint8_t *val, size_t len)
{
    if (chr < CHR_CONFIG || chr > CHR_STATUS || len == 0) {
        return;
    }
    if (!g.connected || !g.secure || !g.sub[chr]) {
        return; /* nobody to tell, or the phone has not asked (or the link is not encrypted yet) */
    }
    if (!bleu_fifo_push(&g.tx, chr, val, (uint16_t)len)) {
        ESP_LOGW(TAG, "notification queue full: dropped (%u so far)", (unsigned)g.tx.drops);
    }
}

bool blec_pump(void)
{
    static uint8_t rec[BLEU_FIFO_REC_MAX];
    uint8_t chr;
    uint16_t len;
    for (int i = 0; i < PUMP_BATCH && bleu_fifo_peek(&g.tx, &chr, rec, sizeof(rec), &len); i++) {
        if (!g.connected || !g.secure || !g.sub[chr]) {
            bleu_fifo_drop(&g.tx); /* the phone left, or unsubscribed, meanwhile */
            continue;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(rec, len);
        if (om == NULL) {
            break; /* out of buffers: try again at the next pass */
        }
        const int rc = ble_gatts_notify_custom(g.conn_pub, g.val_h[chr], om); /* consumes om */
        if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
            break;
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "notify failed: %d", rc);
        } else {
            g.tx_bytes += len;
        }
        bleu_fifo_drop(&g.tx);
    }
    return !bleu_fifo_empty(&g.tx);
}

void blec_set_read_value(uint8_t chr, const uint8_t *val, size_t len)
{
    if ((chr != CHR_CAPABILITIES && chr != CHR_STATUS) || len > READ_MAX) {
        return;
    }
    const int i = chr == CHR_CAPABILITIES ? 0 : 1;
    portENTER_CRITICAL(&g.lock);
    memcpy(g.rd[i], val, len);
    g.rd_len[i] = (uint16_t)len;
    portEXIT_CRITICAL(&g.lock);
}

void blec_set_battery(uint8_t pct)
{
    if (pct == g.battery) {
        return;
    }
    g.battery = pct;
    if (pct <= 100 && g.connected && g.secure && g.sub[CHR_BATTERY]) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&pct, 1);
        if (om != NULL) {
            ble_gatts_notify_custom(g.conn_pub, g.val_h[CHR_BATTERY], om);
        }
    }
}

void blec_set_pairing_mode(bool on)
{
    const bool opening = on && !g.pairing_mode;
    g.pairing_mode = on; /* the host task's tick adjusts the advertising interval */
    if (opening && g.connected) {
        /* One connection means an old phone would otherwise keep the board from advertising to the new one. */
        blec_disconnect();
    }
}

void blec_set_busy(bool busy)
{
    g.busy = busy;
}

bool blec_set_idle_params(const bleu_conn_params_t *p)
{
    if (!bleu_set_relaxed_override(p)) {
        return false;
    }
    g.reapply = true; /* the host task's tick asks again: its state is not ours to touch */
    return true;
}

void blec_set_tick_ms(uint32_t ms)
{
    tick_ms = ms == 0 ? TICK_MS : ms; /* taken at the next tick */
}

bool blec_connected(void)
{
    return g.connected;
}

bool blec_ancs_write(const uint8_t *cmd, size_t len)
{
    return g.running && g.cfg.ancs && ancsg_write_cp(cmd, len);
}

/* ---------------------------------------------------------------- GATT table */

static const ble_uuid128_t UUID_SERVICE = BLE_UUID128_INIT(PROTO_UUID_SERVICE_LE);
static const ble_uuid128_t UUID_CAPABILITIES = BLE_UUID128_INIT(PROTO_UUID_CHR_CAPABILITIES_LE);
static const ble_uuid128_t UUID_CONFIG = BLE_UUID128_INIT(PROTO_UUID_CHR_CONFIG_LE);
static const ble_uuid128_t UUID_NOTIFICATION = BLE_UUID128_INIT(PROTO_UUID_CHR_NOTIFICATION_LE);
static const ble_uuid128_t UUID_TRANSFER = BLE_UUID128_INIT(PROTO_UUID_CHR_TRANSFER_LE);
static const ble_uuid128_t UUID_STATUS = BLE_UUID128_INIT(PROTO_UUID_CHR_STATUS_LE);

/* gatt.json: "encrypted_bonded" = encryption, an authenticated (MITM) key and a 128-bit key size; that the
 * link is bonded is checked when it is encrypted (gap_event). */
#define SEC_R (BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN)
#define SEC_W (BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN)

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);

#define ARG(id) ((void *)(uintptr_t)(id))
static struct ble_gatt_chr_def epager_chrs[] = {
    {.uuid = &UUID_CAPABILITIES.u, .access_cb = access_cb, .arg = ARG(CHR_CAPABILITIES), .flags = BLE_GATT_CHR_F_READ,
     .val_handle = &g.val_h[CHR_CAPABILITIES]},
    {.uuid = &UUID_CONFIG.u, .access_cb = access_cb, .arg = ARG(CHR_CONFIG), .min_key_size = 16,
     .flags = BLE_GATT_CHR_F_WRITE | SEC_W | BLE_GATT_CHR_F_NOTIFY, .val_handle = &g.val_h[CHR_CONFIG]},
    {.uuid = &UUID_NOTIFICATION.u, .access_cb = access_cb, .arg = ARG(CHR_NOTIFICATION), .min_key_size = 16,
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | SEC_W | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &g.val_h[CHR_NOTIFICATION]},
    {.uuid = &UUID_TRANSFER.u, .access_cb = access_cb, .arg = ARG(CHR_TRANSFER), .min_key_size = 16,
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | SEC_W | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &g.val_h[CHR_TRANSFER]},
    {.uuid = &UUID_STATUS.u, .access_cb = access_cb, .arg = ARG(CHR_STATUS), .min_key_size = 16,
     .flags = BLE_GATT_CHR_F_READ | SEC_R | BLE_GATT_CHR_F_NOTIFY, .val_handle = &g.val_h[CHR_STATUS]},
    {0},
};

static const ble_uuid16_t UUID_BATTERY_SERVICE = BLE_UUID16_INIT(0x180F), UUID_BATTERY_LEVEL = BLE_UUID16_INIT(0x2A19),
                          UUID_DIS = BLE_UUID16_INIT(0x180A), UUID_MANUFACTURER = BLE_UUID16_INIT(0x2A29),
                          UUID_MODEL = BLE_UUID16_INIT(0x2A24), UUID_FIRMWARE = BLE_UUID16_INIT(0x2A26);

static struct ble_gatt_chr_def battery_chrs[] = {
    {.uuid = &UUID_BATTERY_LEVEL.u, .access_cb = access_cb, .arg = ARG(CHR_BATTERY), .min_key_size = 16,
     .flags = BLE_GATT_CHR_F_READ | SEC_R | BLE_GATT_CHR_F_NOTIFY, .val_handle = &g.val_h[CHR_BATTERY]},
    {0},
};

static struct ble_gatt_chr_def dis_chrs[] = {
    {.uuid = &UUID_MANUFACTURER.u, .access_cb = access_cb, .arg = ARG(CHR_DIS_MANUF), .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &UUID_MODEL.u, .access_cb = access_cb, .arg = ARG(CHR_DIS_MODEL), .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &UUID_FIRMWARE.u, .access_cb = access_cb, .arg = ARG(CHR_DIS_FW), .flags = BLE_GATT_CHR_F_READ},
    {0},
};

static const struct ble_gatt_svc_def svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &UUID_SERVICE.u, .characteristics = epager_chrs},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &UUID_BATTERY_SERVICE.u, .characteristics = battery_chrs},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &UUID_DIS.u, .characteristics = dis_chrs},
    {0},
};

static int append_str(struct ble_gatt_access_ctxt *ctxt, const char *s)
{
    s = s != NULL ? s : "";
    return os_mbuf_append(ctxt->om, s, strlen(s)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    static uint8_t wbuf[1u + BLEC_WRITE_MAX]; /* host task only */
    (void)attr;
    const unsigned id = (unsigned)(uintptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        switch (id) {
        case CHR_CAPABILITIES:
        case CHR_STATUS: {
            const int i = id == CHR_CAPABILITIES ? 0 : 1;
            static uint8_t tmp[READ_MAX]; /* host task only */
            portENTER_CRITICAL(&g.lock);
            const uint16_t n = g.rd_len[i];
            memcpy(tmp, g.rd[i], n);
            portEXIT_CRITICAL(&g.lock);
            return os_mbuf_append(ctxt->om, tmp, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        case CHR_BATTERY: {
            const uint8_t b = g.battery;
            if (b > 100) {
                return BLE_ATT_ERR_UNLIKELY; /* no battery reader */
            }
            return os_mbuf_append(ctxt->om, &b, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        case CHR_DIS_MANUF:
            return append_str(ctxt, g.cfg.manufacturer);
        case CHR_DIS_MODEL:
            return append_str(ctxt, g.cfg.model);
        case CHR_DIS_FW:
            return append_str(ctxt, g.cfg.firmware);
        default:
            return BLE_ATT_ERR_UNLIKELY;
        }
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && id >= CHR_CONFIG && id <= CHR_TRANSFER) {
        uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
        if (n > BLEC_WRITE_MAX) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (!g.secure) {
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN; /* ATT already refused this; defence in depth */
        }
        wbuf[0] = (uint8_t)id;
        if (ble_hs_mbuf_to_flat(ctxt->om, wbuf + 1, BLEC_WRITE_MAX, &n) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        g.rx_bytes += n;
        if (id == CHR_TRANSFER) {
            bleu_policy_activity(&g.policy, now_ms());
        }
        /* A full queue refuses the write (the phone may retry); write-without-response data is lost, which the
         * transfer protocol's offsets and acknowledgements recover from. */
        return post(BLEC_EV_WRITE, wbuf, 1u + n) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    (void)conn;
    return BLE_ATT_ERR_UNLIKELY;
}

/* ---------------------------------------------------------------- GAP */

static int gap_event(struct ble_gap_event *event, void *arg);

static uint8_t bond_count(void)
{
    int n = 0;
    return ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n) == 0 ? (uint8_t)n : 0;
}

uint8_t blec_bond_count(void)
{
    if (g.running) {
        g.bonds_cached = bond_count();
    }
    return g.bonds_cached;
}

static bool peer_bonded(const ble_addr_t *id_addr)
{
    ble_addr_t peers[MAX_PEERS];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, MAX_PEERS) != 0) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (ble_addr_cmp(&peers[i], id_addr) == 0) {
            return true;
        }
    }
    return false;
}

static void adv_start(void)
{
    if (!g.started || g.connected || g.halting) {
        return;
    }
    ble_gap_adv_stop(); /* restarting with another interval: ignore "not advertising" */
    g.adv_units = bleu_adv_interval(g.pairing_mode, now_ms() - g.adv_start_ms);
    const struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                         .itvl_min = g.adv_units, .itvl_max = g.adv_units};
    const int rc = ble_gap_adv_start(g.own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && !g.adv_failed) { /* once: the tick tries again every 250 ms */
        ESP_LOGE(TAG, "advertising did not start: %d", rc);
    }
    g.adv_failed = rc != 0;
    g.adv_on = rc == 0;
}

/* False if the request was not sent: the policy then asks again at the next tick. */
static bool apply_conn_mode(bleu_conn_mode_t mode)
{
    bleu_conn_params_t cp;
    bleu_conn_params(mode, &cp);
    const struct ble_gap_upd_params u = {.itvl_min = cp.itvl_min, .itvl_max = cp.itvl_max, .latency = cp.latency,
                                         .supervision_timeout = cp.timeout};
    const int rc = ble_gap_update_params(g.conn, &u);
#if CONFIG_EPAGER_BLE_STATS
    ESP_LOGI(TAG, "asked for the %s connection parameters: %d", mode == BLEU_CONN_FAST ? "fast" : "relaxed", rc);
#endif
    return rc == 0;
}

static void tick_cb(struct ble_npl_event *ev)
{
    (void)ev;
    if (g.halting) {
        return; /* blec_stop: no re-arm */
    }
    const uint32_t t = now_ms();
    if (!g.connected) {
        const uint16_t want = bleu_adv_interval(g.pairing_mode, t - g.adv_start_ms);
        if (g.started && (!g.adv_on || want != g.adv_units)) {
            adv_start();
        }
    } else {
        if (!g.secure && bleu_drop_unsecured(t - g.connect_ms, g.pairing_mode, g.peer_was_bonded)) {
            if (!g.dropping) {
                g.dropping = true;
                ESP_LOGI(TAG, "dropping a connection that did not secure the link");
                ble_gap_terminate(g.conn, BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            bleu_conn_mode_t mode;
            if (g.reapply) {
                g.reapply = false;
                bleu_policy_activity(&g.policy, t); /* fast for a while, then the new idle parameters */
            }
            if (g.busy) {
                bleu_policy_activity(&g.policy, t);
            }
            if (bleu_policy_step(&g.policy, t, &mode) && !apply_conn_mode(mode)) {
                g.policy.mode = mode == BLEU_CONN_FAST ? BLEU_CONN_RELAXED : BLEU_CONN_FAST; /* not sent: ask again */
            }
        }
#if CONFIG_EPAGER_BLE_STATS
        if (t - g.stat_ms >= 1000 && (g.rx_bytes || g.tx_bytes)) {
            ESP_LOGI(TAG, "link: %u B/s received, %u B/s sent", (unsigned)g.rx_bytes, (unsigned)g.tx_bytes);
            g.rx_bytes = g.tx_bytes = 0;
            g.stat_ms = t;
        }
#endif
    }
    ble_npl_callout_reset(&g.tick, ble_npl_time_ms_to_ticks32(tick_ms));
}

static void terminate(uint16_t conn)
{
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            adv_start();
            return 0;
        }
        g.conn = g.conn_pub = ev->connect.conn_handle;
        g.connected = true;
        g.secure = false;
        g.pairing_active = false;
        g.peer_was_bonded = false;
        g.dropping = false;
        memset((void *)g.sub, 0, sizeof(g.sub));
        g.adv_on = false;
        g.connect_ms = now_ms();
        bleu_policy_connect(&g.policy, g.connect_ms);
        post_u16(BLEC_EV_CONNECTED, ble_att_mtu(g.conn));
        ESP_LOGI(TAG, "phone connected");
        if (ble_gap_conn_find(g.conn, &desc) == 0) {
            g.peer_was_bonded = peer_bonded(&desc.peer_id_addr);
            if (bleu_initiate_security(g.pairing_mode, g.peer_was_bonded)) {
                /* A known phone encrypts again normally; pairing mode instead asks a new phone for a bond. */
                ble_gap_security_initiate(g.conn);
            }
        }
        ble_gattc_exchange_mtu(g.conn, NULL, NULL);
#if CONFIG_EPAGER_BLE_2M_PHY
        ble_gap_set_prefered_le_phy(g.conn, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
#endif
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected (reason %d)", ev->disconnect.reason);
        g.connected = g.secure = g.peer_was_bonded = false;
        g.conn = g.conn_pub = BLE_HS_CONN_HANDLE_NONE;
        memset((void *)g.sub, 0, sizeof(g.sub));
        bleu_policy_disconnect(&g.policy);
        if (g.cfg.ancs) {
            ancsg_stop();
        }
        if (g.pairing_active) {
            g.pairing_active = false;
            post_u8(BLEC_EV_PAIR_DONE, 0);
        }
        const uint8_t reason[2] = {(uint8_t)(ev->disconnect.reason & 0xFF), (uint8_t)(ev->disconnect.reason >> 8)};
        post(BLEC_EV_DISCONNECTED, reason, sizeof(reason));
        g.adv_start_ms = now_ms(); /* fast advertising again: the phone is probably coming back */
        adv_start();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        g.adv_on = false;
        adv_start();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        const bool pairing = g.pairing_active;
        g.pairing_active = false;
        if (ev->enc_change.status != 0) {
            ESP_LOGI(TAG, "encryption failed: %d", ev->enc_change.status);
            if (pairing) {
                post_u8(BLEC_EV_PAIR_DONE, 0);
            }
            terminate(ev->enc_change.conn_handle);
            return 0;
        }
        if (ble_gap_conn_find(ev->enc_change.conn_handle, &desc) != 0) {
            return 0;
        }
        if (g.pairing_mode && g.peer_was_bonded && !pairing) {
            /* A valid old bond must not occupy the sole connection while the user is adding another phone. */
            ESP_LOGI(TAG, "bonded reconnect deferred while pairing mode is open");
            terminate(ev->enc_change.conn_handle);
            return 0;
        }
        /* PLAN D6: encrypted, authenticated (the passkey), 128-bit key (Secure Connections only is set in
         * sdkconfig) and bonded. Anything less is refused. */
        if (!desc.sec_state.encrypted || !desc.sec_state.authenticated || !desc.sec_state.bonded ||
            desc.sec_state.key_size != 16) {
            ESP_LOGW(TAG, "link refused: encrypted %d authenticated %d bonded %d key %d", desc.sec_state.encrypted,
                     desc.sec_state.authenticated, desc.sec_state.bonded, desc.sec_state.key_size);
            if (pairing) {
                post_u8(BLEC_EV_PAIR_DONE, 0);
            }
            /* A phone that pairs as "no input, no output" (Just Works) gets a bond persisted before this event,
             * outside pairing mode too, and a full store would then drop a real phone for it. Delete it. */
            ble_store_util_delete_peer(&desc.peer_id_addr);
            terminate(ev->enc_change.conn_handle);
            return 0;
        }
        g.secure = true;
        g.peer_was_bonded = true;
        post(BLEC_EV_SECURE, NULL, 0);
        post_u8(BLEC_EV_BONDS, bond_count());
        if (pairing) {
            post_u8(BLEC_EV_PAIR_DONE, 1);
        }
        ESP_LOGI(TAG, "link secure%s", pairing ? " (new bond)" : "");
        if (g.cfg.ancs) {
            ancsg_start(ev->enc_change.conn_handle);
        }
        return 0;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        const uint16_t conn = ev->passkey.conn_handle;
        if (ev->passkey.params.action != BLE_SM_IOACT_DISP || !bleu_pairing_allowed(g.pairing_mode, false)) {
            ESP_LOGI(TAG, "pairing refused (%s)", g.pairing_mode ? "unsupported method" : "not in pairing mode");
            terminate(conn);
            return 0;
        }
        uint32_t pk;
        while (!bleu_passkey(esp_random(), &pk)) {
        }
        struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP, .passkey = pk};
        if (ble_sm_inject_io(conn, &io) != 0) {
            terminate(conn);
            return 0;
        }
        g.pairing_active = true;
        const uint8_t b[4] = {(uint8_t)pk, (uint8_t)(pk >> 8), (uint8_t)(pk >> 16), (uint8_t)(pk >> 24)};
        post(BLEC_EV_PASSKEY, b, sizeof(b)); /* never logged */
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The phone is bonded here but starts pairing again (it forgot us). Only in pairing mode do we replace
         * the old bond. */
        if (!g.pairing_mode) {
            ESP_LOGI(TAG, "repeat pairing refused: not in pairing mode");
            terminate(ev->repeat_pairing.conn_handle);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        g.peer_was_bonded = false;
        g.connect_ms = now_ms(); /* the replacement bond now gets the normal passkey-entry window */
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_SUBSCRIBE:
        for (unsigned i = CHR_CONFIG; i <= CHR_BATTERY; i++) {
            if (g.val_h[i] == ev->subscribe.attr_handle) {
                g.sub[i] = ev->subscribe.cur_notify;
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        post_u16(BLEC_EV_MTU, ev->mtu.value);
#if CONFIG_EPAGER_BLE_STATS
        ESP_LOGI(TAG, "MTU %u", ev->mtu.value);
#endif
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        static uint8_t buf[BLEC_DATA_MAX]; /* host task only: its stack is small */
        uint16_t n = OS_MBUF_PKTLEN(ev->notify_rx.om);
        if (n > sizeof(buf) || ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, sizeof(buf), &n) != 0) {
            return 0;
        }
        if (g.cfg.ancs) {
            ancsg_on_notify_rx(ev->notify_rx.conn_handle, ev->notify_rx.attr_handle, buf, n);
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE:
        bleu_policy_updated(&g.policy, now_ms()); /* the central's, or the answer to ours: wait for quiet */
#if CONFIG_EPAGER_BLE_STATS
        if (ble_gap_conn_find(ev->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "connection: interval %.2f ms, latency %u, timeout %u ms", desc.conn_itvl * 1.25,
                     desc.conn_latency, desc.supervision_timeout * 10);
        }
#endif
        return 0;

#if CONFIG_EPAGER_BLE_STATS
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        ESP_LOGI(TAG, "PHY: tx %u, rx %u", ev->phy_updated.tx_phy, ev->phy_updated.rx_phy);
        return 0;
#endif
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------- bonds */

static void forget_cb(struct ble_npl_event *ev)
{
    (void)ev;
    if (g.connected) {
        ble_gap_terminate(g.conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_store_clear();
    ESP_LOGI(TAG, "all bonds forgotten");
    post_u8(BLEC_EV_BONDS, 0);
}

/* The identity address of the phone on the link, false when none (host task or app task: the host locks). */
static bool link_peer(ble_addr_t *out)
{
    struct ble_gap_conn_desc desc;
    if (!g.connected || ble_gap_conn_find(g.conn_pub, &desc) != 0) {
        return false;
    }
    *out = desc.peer_id_addr;
    return true;
}

size_t blec_bonds(bool *connected, size_t max)
{
    ble_addr_t peers[MAX_PEERS], link;
    int n = 0;
    if (!g.running || ble_store_util_bonded_peers(peers, &n, MAX_PEERS) != 0) {
        return 0;
    }
    const bool have_link = link_peer(&link);
    size_t out = 0;
    for (int i = 0; i < n && out < max; i++, out++) {
        connected[out] = have_link && ble_addr_cmp(&peers[i], &link) == 0;
    }
    return out;
}

static void forget_one_cb(struct ble_npl_event *ev)
{
    (void)ev;
    ble_addr_t peers[MAX_PEERS], link;
    int n = 0;
    const size_t index = g.forget_index;
    if (ble_store_util_bonded_peers(peers, &n, MAX_PEERS) == 0 && index < (size_t)n) {
        if (link_peer(&link) && ble_addr_cmp(&peers[index], &link) == 0) {
            ble_gap_terminate(g.conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        ble_store_util_delete_peer(&peers[index]);
        ESP_LOGI(TAG, "bond %u forgotten", (unsigned)index);
    }
    post_u8(BLEC_EV_BONDS, bond_count());
}

void blec_forget_bond(size_t index)
{
    if (!g.running) {
        return;
    }
    g.forget_index = index;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &g.ev_forget_one);
}

void blec_forget_bonds(void)
{
    if (g.running) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &g.ev_forget);
    }
}

void blec_disconnect(void)
{
    if (g.connected) {
        ble_gap_terminate(g.conn_pub, BLE_ERR_REM_USER_CONN_TERM);
    }
}

/* ---------------------------------------------------------------- start */

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset, reason %d", reason);
    /* Every link is gone and nothing advertises: sync_cb runs again and begin_service() starts over. */
    if (g.connected) {
        g.connected = g.secure = false;
        g.conn = g.conn_pub = BLE_HS_CONN_HANDLE_NONE;
        if (g.cfg.ancs) {
            ancsg_stop();
        }
        post(BLEC_EV_DISCONNECTED, NULL, 0);
    }
    g.synced = g.started = g.adv_on = false;
}

/* Host task: advertising and the periodic tick begin once the host has synced and the app said go. */
static void begin_service(void)
{
    if (!g.synced || !g.enabled || g.started) {
        return;
    }
    uint8_t adv[BLEU_ADV_MAX], rsp[BLEU_ADV_MAX];
    const size_t adv_len = bleu_adv_data(adv, sizeof(adv)), rsp_len = bleu_scan_rsp_data(g.name, rsp, sizeof(rsp));
    if (ble_gap_adv_set_data(adv, (int)adv_len) != 0 || ble_gap_adv_rsp_set_data(rsp, (int)rsp_len) != 0) {
        ESP_LOGE(TAG, "advertising data refused");
        return;
    }
    g.started = true;
    g.adv_start_ms = now_ms();
    post_u8(BLEC_EV_BONDS, bond_count()); /* the app opens pairing mode at the first boot (no bonds) */
    adv_start();
    ble_npl_callout_reset(&g.tick, ble_npl_time_ms_to_ticks32(tick_ms));
    ESP_LOGI(TAG, "advertising as %s, %u bond(s)", g.name, (unsigned)bond_count());
}

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &g.own_addr_type) != 0) {
        ESP_LOGE(TAG, "no Bluetooth address");
        return;
    }
    g.synced = true;
    begin_service();
}

static void start_cb(struct ble_npl_event *ev)
{
    (void)ev;
    begin_service();
}

void blec_start(void)
{
    g.enabled = true;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &g.ev_start);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run(); /* returns only when nimble_port_stop() runs */
    nimble_port_freertos_deinit();
}

static void *alloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != NULL ? p : heap_caps_calloc(1, n, MALLOC_CAP_8BIT);
}

const char *blec_device_name(void)
{
    uint8_t mac[6];
    if (g.name[0] == 0 && esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        const uint8_t le[6] = {mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]}; /* NimBLE's order */
        bleu_device_name(g.name, le);
    }
    return g.name;
}

/* NimBLE, the controller, the GATT table and the host task: at boot, and again after blec_stop. */
static bool stack_up(void)
{
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return false;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr; /* a fourth phone replaces the oldest bond */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;          /* the board shows the passkey (PLAN D6) */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(svcs) != 0 || ble_gatts_add_svcs(svcs) != 0) {
        ESP_LOGE(TAG, "GATT table refused");
        nimble_port_deinit();
        return false;
    }
    ble_svc_gap_device_name_set(g.name);
    ble_store_config_init();
    if (g.cfg.ancs) {
        ancsg_init(ancs_post);
    }
    ble_npl_callout_init(&g.tick, nimble_port_get_dflt_eventq(), tick_cb, NULL);
    ble_npl_event_init(&g.ev_forget, forget_cb, NULL);
    ble_npl_event_init(&g.ev_forget_one, forget_one_cb, NULL);
    ble_npl_event_init(&g.ev_start, start_cb, NULL);
    g.halting = false;
    g.running = true;
    nimble_port_freertos_init(host_task);
    return true;
}

bool blec_init(const blec_config_t *cfg)
{
    g.cfg = *cfg;
    blec_device_name();
    uint8_t *rx = alloc(RX_FIFO_BYTES), *tx = alloc(TX_FIFO_BYTES);
    if (rx == NULL || tx == NULL) {
        return false;
    }
    bleu_fifo_init(&g.rx, rx, RX_FIFO_BYTES);
    bleu_fifo_init(&g.tx, tx, TX_FIFO_BYTES);
    return stack_up();
}

bool blec_running(void)
{
    return g.running;
}

void blec_stop(void)
{
    if (!g.running) {
        return;
    }
    g.bonds_cached = bond_count();
    g.halting = true; /* the tick and every restart of the advertising stop here */
    /* ble_hs_stop inside ends the link with a normal disconnect (gap_event posts BLEC_EV_DISCONNECTED and stops
     * ANCS) and stops the advertising; the host task then leaves nimble_port_run and deletes itself. */
    if (nimble_port_stop() != 0) {
        ESP_LOGE(TAG, "nimble_port_stop failed: Bluetooth stays on");
        g.halting = false;
        ble_npl_callout_reset(&g.tick, ble_npl_time_ms_to_ticks32(tick_ms));
        return;
    }
    ble_npl_callout_deinit(&g.tick);
    if (g.cfg.ancs) {
        ancsg_deinit();
    }
    if (nimble_port_deinit() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit failed");
    }
    g.running = false;
    g.synced = g.started = g.adv_on = g.adv_failed = g.dropping = false;
    g.connected = g.secure = false;
    g.conn = g.conn_pub = BLE_HS_CONN_HANDLE_NONE;
    memset((void *)g.sub, 0, sizeof(g.sub));
    ESP_LOGI(TAG, "off (quiet hours)");
}

bool blec_resume(void)
{
    if (g.running) {
        return true;
    }
    if (!stack_up()) {
        return false;
    }
    blec_start(); /* advertising begins once the host has synced */
    ESP_LOGI(TAG, "on again");
    return true;
}
