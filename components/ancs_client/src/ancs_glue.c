#include "epager/ancs_glue.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"

#include "epager/proto_gen.h"

#define TAG "ancs"

static const ble_uuid128_t UUID_SERVICE = BLE_UUID128_INIT(PROTO_UUID_ANCS_SERVICE_LE);
static const ble_uuid128_t UUID_NS = BLE_UUID128_INIT(PROTO_UUID_ANCS_NOTIFICATION_SOURCE_LE);
static const ble_uuid128_t UUID_CP = BLE_UUID128_INIT(PROTO_UUID_ANCS_CONTROL_POINT_LE);
static const ble_uuid128_t UUID_DS = BLE_UUID128_INIT(PROTO_UUID_ANCS_DATA_SOURCE_LE);
static const ble_uuid16_t UUID_GATT_SERVICE = BLE_UUID16_INIT(0x1801);
static const ble_uuid16_t UUID_SERVICE_CHANGED = BLE_UUID16_INIT(0x2A05);
static const ble_uuid16_t UUID_CCCD = BLE_UUID16_INIT(0x2902);

#define N_CHR 3 /* Notification Source, Control Point, Data Source */
#define RETRY_FIRST_S 3
#define RETRY_MAX_S 120

typedef enum {
    ST_IDLE = 0,
    ST_SERVICE,   /* discovering the ANCS service */
    ST_CHARS,     /* its characteristics */
    ST_CCC_NS,    /* the Notification Source's descriptors */
    ST_CCC_DS,
    ST_SUB_DS,    /* Data Source first: its answers must have somewhere to go before the replay starts */
    ST_SUB_NS,
    ST_READY,
} state_t;

static struct {
    ancsg_post_fn post;
    uint16_t conn;
    bool running, ready_posted, sc_tried;
    volatile uint16_t conn_pub, cp_pub; /* for ancsg_write_cp, which runs on another task; 0 = not ready */
    state_t st;
    uint16_t svc_start, svc_end;
    uint16_t def[N_CHR], val[N_CHR]; /* indexed by which: 0 NS, 1 CP, 2 DS */
    uint16_t ccc_ns, ccc_ds;
    uint16_t sc_val, sc_svc_start, sc_svc_end;
    uint8_t attempt;
    struct ble_npl_callout retry;
    bool retry_init;
} g = {.conn = BLE_HS_CONN_HANDLE_NONE};

enum { NS = 0, CP = 1, DS = 2 };

static void begin(void);

static void post(ancsg_event_t ev, const uint8_t *data, size_t len)
{
    if (g.post) {
        g.post(ev, data, len);
    }
}

static volatile uint32_t retry_max_s = RETRY_MAX_S;

void ancsg_set_retry_max_s(uint32_t s) { retry_max_s = s; }

static void schedule_retry(const char *why)
{
    if (!g.running) {
        return;
    }
    const uint32_t max_s = retry_max_s;
    if (max_s == 0) {
        g.st = ST_IDLE;
        ESP_LOGI(TAG, "%s; not trying again (retries are off)", why);
        return;
    }
    const uint32_t s = g.attempt < 6 ? (uint32_t)RETRY_FIRST_S << g.attempt : max_s; /* then every max_s while linked */
    const uint32_t wait = s > max_s ? max_s : s;
    if (g.attempt < 250) {
        g.attempt++;
    }
    g.st = ST_IDLE;
    ESP_LOGI(TAG, "%s; trying again in %u s", why, (unsigned)wait);
    ble_npl_callout_reset(&g.retry, ble_npl_time_ms_to_ticks32(wait * 1000u));
}

static void retry_cb(struct ble_npl_event *ev)
{
    (void)ev;
    if (g.running && g.st != ST_READY) {
        begin();
    }
}

static void lost(void)
{
    g.cp_pub = 0;
    if (g.ready_posted) {
        g.ready_posted = false;
        post(ANCSG_EV_LOST, NULL, 0);
    }
}

/* ---------------------------------------------------------------- Service Changed (best effort) */

static int sc_write_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    ESP_LOGI(TAG, "Service Changed indications %s", err->status == 0 ? "on" : "refused");
    return 0;
}

static int sc_dsc_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                     void *arg)
{
    (void)chr_val;
    (void)arg;
    if (err->status == 0 && ble_uuid_cmp(&dsc->uuid.u, &UUID_CCCD.u) == 0) {
        const uint8_t indicate[2] = {2, 0};
        ble_gattc_write_flat(conn, dsc->handle, indicate, sizeof(indicate), sc_write_cb, NULL);
    }
    return 0;
}

static int sc_chr_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (err->status == 0 && ble_uuid_cmp(&chr->uuid.u, &UUID_SERVICE_CHANGED.u) == 0) {
        g.sc_val = chr->val_handle;
        ble_gattc_disc_all_dscs(conn, chr->val_handle, g.sc_svc_end, sc_dsc_cb, NULL);
    }
    return 0;
}

static int sc_svc_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (err->status == 0 && svc != NULL) {
        g.sc_svc_start = svc->start_handle;
        g.sc_svc_end = svc->end_handle;
    } else if (err->status == BLE_HS_EDONE && g.sc_svc_end != 0) {
        ble_gattc_disc_all_chrs(conn, g.sc_svc_start, g.sc_svc_end, sc_chr_cb, NULL);
    }
    return 0;
}

/* iOS tells a client when its GATT database changes (ANCS appears after bonding, or the user allows it). */
static void watch_service_changed(void)
{
    if (g.sc_tried) {
        return;
    }
    g.sc_tried = true;
    g.sc_svc_end = 0;
    ble_gattc_disc_svc_by_uuid(g.conn, &UUID_GATT_SERVICE.u, sc_svc_cb, NULL);
}

/* ---------------------------------------------------------------- discovery and subscription */

static void become_ready(void)
{
    g.st = ST_READY;
    g.attempt = 0;
    g.ready_posted = true;
    g.conn_pub = g.conn;
    g.cp_pub = g.val[CP];
    ESP_LOGI(TAG, "subscribed: a new ANCS session");
    post(ANCSG_EV_READY, NULL, 0);
    watch_service_changed(); /* also when ANCS was there at first: it may go and come back */
}

static int sub_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (!g.running || g.st == ST_READY) { /* (READY: the first notification overtook this answer) */
        return 0;
    }
    if (err->status != 0) { /* typically: the user has not allowed notifications for this accessory yet */
        schedule_retry("subscribing was refused");
        return 0;
    }
    if (g.st == ST_SUB_DS) {
        g.st = ST_SUB_NS;
        const uint8_t on[2] = {1, 0};
        if (ble_gattc_write_flat(g.conn, g.ccc_ns, on, sizeof(on), sub_cb, NULL) != 0) {
            schedule_retry("could not subscribe to the Notification Source");
        }
    } else if (g.st == ST_SUB_NS) {
        become_ready();
    }
    return 0;
}

static void subscribe(void)
{
    const uint8_t on[2] = {1, 0};
    g.st = ST_SUB_DS;
    if (ble_gattc_write_flat(g.conn, g.ccc_ds, on, sizeof(on), sub_cb, NULL) != 0) {
        schedule_retry("could not subscribe to the Data Source");
    }
}

/* The end of the range in which characteristic `which` owns descriptors: the next characteristic's declaration.
 * NimBLE's ble_gattc_disc_all_dscs() takes the characteristic's VALUE handle as its start and asks from the
 * handle after it (found on the iPad, 2026-10-14: starting at value + 1 made the first request empty, EINVAL). */
static uint16_t dsc_end(int which)
{
    uint16_t end = g.svc_end;
    for (int i = 0; i < N_CHR; i++) {
        if (g.def[i] > g.val[which] && g.def[i] - 1 < end) {
            end = g.def[i] - 1;
        }
    }
    return end;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    (void)conn;
    (void)chr_val;
    (void)arg;
    if (!g.running) {
        return 0;
    }
    const bool ns = g.st == ST_CCC_NS;
    if (err->status == 0 && dsc != NULL) {
        if (ble_uuid_cmp(&dsc->uuid.u, &UUID_CCCD.u) == 0) {
            if (ns) {
                g.ccc_ns = dsc->handle;
            } else {
                g.ccc_ds = dsc->handle;
            }
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        schedule_retry("descriptor discovery failed");
        return 0;
    }
    if (ns) {
        if (g.ccc_ns == 0) {
            schedule_retry("no Notification Source descriptor");
            return 0;
        }
        g.st = ST_CCC_DS;
        if (ble_gattc_disc_all_dscs(g.conn, g.val[DS], dsc_end(DS), dsc_cb, NULL) != 0) {
            schedule_retry("descriptor discovery failed");
        }
    } else if (g.ccc_ds == 0) {
        schedule_retry("no Data Source descriptor");
    } else {
        subscribe();
    }
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn;
    (void)arg;
    if (!g.running) {
        return 0;
    }
    if (err->status == 0 && chr != NULL) {
        const ble_uuid_t *want[N_CHR] = {&UUID_NS.u, &UUID_CP.u, &UUID_DS.u};
        for (int i = 0; i < N_CHR; i++) {
            if (ble_uuid_cmp(&chr->uuid.u, want[i]) == 0) {
                g.def[i] = chr->def_handle;
                g.val[i] = chr->val_handle;
            }
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        schedule_retry("characteristic discovery failed");
    } else if (!g.val[NS] || !g.val[CP] || !g.val[DS]) {
        schedule_retry("the service lacks a characteristic");
    } else {
        g.st = ST_CCC_NS;
        if (ble_gattc_disc_all_dscs(g.conn, g.val[NS], dsc_end(NS), dsc_cb, NULL) != 0) {
            schedule_retry("descriptor discovery failed");
        }
    }
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn;
    (void)arg;
    if (!g.running) {
        return 0;
    }
    if (err->status == 0 && svc != NULL) {
        g.svc_start = svc->start_handle;
        g.svc_end = svc->end_handle;
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        schedule_retry("service discovery failed");
        return 0;
    }
    if (g.svc_end == 0) { /* not there (yet): iOS offers ANCS after bonding and the user's consent */
        watch_service_changed();
        schedule_retry("no ANCS on this phone");
        return 0;
    }
    g.st = ST_CHARS;
    if (ble_gattc_disc_all_chrs(g.conn, g.svc_start, g.svc_end, chr_cb, NULL) != 0) {
        schedule_retry("characteristic discovery failed");
    }
    return 0;
}

static void begin(void)
{
    g.svc_start = g.svc_end = 0;
    memset(g.def, 0, sizeof(g.def));
    memset(g.val, 0, sizeof(g.val));
    g.ccc_ns = g.ccc_ds = 0;
    g.st = ST_SERVICE;
    if (ble_gattc_disc_svc_by_uuid(g.conn, &UUID_SERVICE.u, svc_cb, NULL) != 0) {
        schedule_retry("could not start service discovery");
    }
}

/* ---------------------------------------------------------------- API */

void ancsg_init(ancsg_post_fn post_fn)
{
    g.post = post_fn;
}

void ancsg_start(uint16_t conn)
{
    if (!g.retry_init) {
        ble_npl_callout_init(&g.retry, nimble_port_get_dflt_eventq(), retry_cb, NULL);
        g.retry_init = true;
    }
    ancsg_stop();
    g.running = true;
    g.conn = conn;
    g.attempt = 0;
    g.sc_tried = false;
    g.sc_val = 0;
    begin();
}

void ancsg_deinit(void)
{
    ancsg_stop();
    if (g.retry_init) {
        ble_npl_callout_deinit(&g.retry);
        g.retry_init = false;
    }
}

void ancsg_stop(void)
{
    if (g.retry_init) {
        ble_npl_callout_stop(&g.retry);
    }
    lost();
    g.running = false;
    g.st = ST_IDLE;
    g.conn = BLE_HS_CONN_HANDLE_NONE;
}

bool ancsg_ready(void)
{
    return g.running && g.st == ST_READY;
}

bool ancsg_on_notify_rx(uint16_t conn, uint16_t attr_handle, const uint8_t *data, uint16_t len)
{
    if (!g.running || conn != g.conn) {
        return false;
    }
    if (g.st == ST_SUB_NS && attr_handle == g.val[NS]) {
        become_ready(); /* iOS began the replay before the write response reached us */
    }
    if (g.st == ST_READY && attr_handle == g.val[NS]) {
        post(ANCSG_EV_NS, data, len);
    } else if (g.st == ST_READY && attr_handle == g.val[DS]) {
        post(ANCSG_EV_DS, data, len);
    } else if (g.sc_val != 0 && attr_handle == g.sc_val) {
        ESP_LOGI(TAG, "the phone's services changed");
        lost();
        g.attempt = 0;
        g.sc_tried = true; /* the subscription stays */
        ble_npl_callout_stop(&g.retry);
        begin();
    } else {
        return false;
    }
    return true;
}

static int cp_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    /* ATT errors arrive as 0x100 + code; ANCS's own are 0xA0-0xA3 (Apple's ANCS specification) */
    const uint8_t st = err->status == 0 ? 0 : err->status >= 0x100 && err->status < 0x200 ? (uint8_t)err->status : 0xFF;
    post(ANCSG_EV_WRITE, &st, 1);
    return 0;
}

bool ancsg_write_cp(const uint8_t *cmd, size_t len)
{
    const uint16_t conn = g.conn_pub, cp = g.cp_pub; /* a snapshot: the host task may rediscover meanwhile */
    if (cp == 0 || len == 0 || len > 512) {
        return false;
    }
    const uint16_t mtu = ble_att_mtu(conn);
    if (mtu >= 3 && len <= (size_t)mtu - 3) {
        return ble_gattc_write_flat(conn, cp, cmd, (uint16_t)len, cp_cb, NULL) == 0;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(cmd, (uint16_t)len); /* longer than one write: prepare/execute */
    return om != NULL && ble_gattc_write_long(conn, cp, 0, om, cp_cb, NULL) == 0;
}
