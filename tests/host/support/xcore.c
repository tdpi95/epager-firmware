#include "xcore.h"

#include <stdlib.h>
#include <string.h>

#include "test_support.h"

static uint32_t now_ms(void *ctx) { return ((xcore_t *)ctx)->now_ms; }

static bool read_battery(void *ctx, uint8_t *pct, uint8_t *ext)
{
    xcore_t *x = ctx;
    *pct = x->battery_pct;
    *ext = x->external_power;
    return x->battery_ok;
}

static bool rtc_get(void *ctx, uint32_t *utc)
{
    xcore_t *x = ctx;
    *utc = x->rtc_utc ? x->rtc_utc + (x->now_ms - x->rtc_at_ms) / 1000u : 0;
    return x->rtc_utc != 0;
}

static bool rtc_set(void *ctx, uint32_t utc)
{
    xcore_t *x = ctx;
    x->rtc_utc = utc;
    x->rtc_at_ms = x->now_ms;
    x->rtc_writes++;
    return true;
}

static void notify(void *ctx, uint8_t chr, const uint8_t *val, size_t len)
{
    xcore_t *x = ctx;
    if (x->n_out < XCORE_MAX_OUT && len <= sizeof(x->out[0].v)) {
        x->out[x->n_out].chr = chr;
        x->out[x->n_out].len = (uint16_t)len;
        memcpy(x->out[x->n_out].v, val, len);
        x->n_out++;
    }
}

static void on_dismissed(void *ctx, const npipe_dismissed_t *d)
{
    xcore_t *x = ctx;
    x->n_dismissed++;
    x->last_dismissed = *d;
}

static void on_notif(void *ctx, const npipe_notif_t *n)
{
    xcore_t *x = ctx;
    x->n_notif_events++;
    x->last_notif = *n;
}

static void on_setting(void *ctx, core_setting_t kind)
{
    ((xcore_t *)ctx)->n_setting[kind]++;
}

static void on_config(void *ctx, const uint16_t *keys, size_t n)
{
    (void)keys;
    (void)n;
    ((xcore_t *)ctx)->n_config_events++;
}

static proto_status_t wifi_set(void *ctx, const uint8_t *ssid, size_t ssid_len, const uint8_t *pw, size_t pw_len,
                               bool forget)
{
    xcore_t *x = ctx;
    x->wifi_sets++;
    if (x->wifi_result != PROTO_OK) {
        return x->wifi_result;
    }
    char name[CORE_SSID_MAX + 1] = "";
    if (ssid) {
        snprintf(name, sizeof(name), "%.*s", (int)ssid_len, (const char *)ssid);
    }
    size_t at = x->wifi_profile_count;
    for (size_t i = 0; i < x->wifi_profile_count; i++) {
        if (strcmp(x->wifi_profiles[i], name) == 0) {
            at = i;
            break;
        }
    }
    if (forget) {
        if (!ssid) {
            x->wifi_profile_count = 0;
            x->wifi_ssid[0] = '\0';
        } else if (at < x->wifi_profile_count) {
            memmove(&x->wifi_profiles[at], &x->wifi_profiles[at + 1],
                    (x->wifi_profile_count - at - 1) * sizeof(x->wifi_profiles[0]));
            x->wifi_profile_count--;
            x->wifi_profiles[x->wifi_profile_count][0] = '\0';
        }
    } else {
        if (at < x->wifi_profile_count) {
            memmove(&x->wifi_profiles[at], &x->wifi_profiles[at + 1],
                    (x->wifi_profile_count - at - 1) * sizeof(x->wifi_profiles[0]));
            x->wifi_profile_count--;
        } else if (x->wifi_profile_count == 3) {
            memmove(&x->wifi_profiles[0], &x->wifi_profiles[1], 2 * sizeof(x->wifi_profiles[0]));
            x->wifi_profile_count--;
        }
        snprintf(x->wifi_profiles[x->wifi_profile_count++], sizeof(x->wifi_profiles[0]), "%s", name);
        snprintf(x->wifi_ssid, sizeof(x->wifi_ssid), "%s", name);
    }
    snprintf(x->wifi_pw, sizeof(x->wifi_pw), "%.*s", (int)pw_len, pw ? (const char *)pw : "");
    return PROTO_OK;
}

static void on_ota(void *ctx, const xfer_ota_event_t *ev) { ((xcore_t *)ctx)->ota_events[ev->kind]++; }

static void setup_params(xcore_t *x)
{
    x->assets[0] = (xfer_asset_support_t){PROTO_ASSET_TYPE_THEME, PROTO_ASSET_FORMAT_THEME_V1, 512, 0, 0, 0, 0};
    x->assets[1] = (xfer_asset_support_t){PROTO_ASSET_TYPE_BACKGROUND, PROTO_ASSET_FORMAT_MONO1, 5016, 0, 0, 0, 0};
    x->params = (core_params_t){
        .device_name = "ePager-1A2B", .board_id = "host_test_board", .firmware_version = x->version,
        .variant = PROTO_VARIANT_PAPER, .display_type = PROTO_DISPLAY_TYPE_EPAPER_BW, .color_depth = 1,
        .refresh_class = PROTO_REFRESH_CLASS_EPAPER, .display_w = 200, .display_h = 200,
        .features = PROTO_FEATURES_ANDROID_NOTIFICATIONS | PROTO_FEATURES_ASSETS | PROTO_FEATURES_OTA |
                    PROTO_FEATURES_BATTERY | (x->has_wifi ? PROTO_FEATURES_WIFI | PROTO_FEATURES_WEATHER : 0u),
        .transports = PROTO_TRANSPORTS_BLE, .max_mtu = 517,
        .kv_cfg = &x->kv.iface, .kv_apps = &x->kv.iface, .kv_xfer = &x->kv.iface,
        .blob = &x->blob.iface, .assets = x->assets, .n_assets = 2, .fw = &x->fw.iface,
        .low_battery_pct = 30, .ota_required_checks = 0, .ota_verify_timeout_ms = 60000,
        .ack_window = 1024, .journal_every = 512,
        .slots = x->slots, .n_slots = 8, .apps = x->apps, .n_apps = 16,
        .now_ms = now_ms, .read_battery = x->has_battery ? read_battery : NULL,
        .battery_poll_ms = 10000, .apps_flush_ms = 60000,
        .rtc_get = x->has_rtc ? rtc_get : NULL, .rtc_set = x->has_rtc ? rtc_set : NULL, .wifi_set = wifi_set,
        .notify = notify, .on_notif = on_notif, .on_dismissed = on_dismissed, .on_config = on_config, .on_setting = on_setting, .on_ota = on_ota, .ctx = x};
}

xcore_t *xcore_new(void)
{
    xcore_t *x = calloc(1, sizeof(*x));
    kv_mem_init(&x->kv);
    blob_mem_init(&x->blob, 16384);
    fw_mem_init(&x->fw, x->fw_buf, XCORE_FW_SLOT);
    x->now_ms = 1000;
    x->has_battery = x->battery_ok = true;
    x->battery_pct = 80;
    x->external_power = PROTO_EXTERNAL_POWER_NO;
    snprintf(x->version, sizeof(x->version), "0.1.0");
    xcore_reboot(x, false, true);
    return x;
}

void xcore_free(xcore_t *x)
{
    blob_mem_free(&x->blob);
    free(x);
}

void xcore_reboot(xcore_t *x, bool image_pending_verify, bool connect)
{
    memset(x->slots, 0, sizeof(x->slots));
    memset(x->apps, 0, sizeof(x->apps));
    setup_params(x);
    core_init(&x->core, &x->params);
    core_boot(&x->core, image_pending_verify);
    if (connect) {
        core_connect(&x->core, 517, true);
    }
    x->n_out = 0;
}

void xcore_send(xcore_t *x, uint8_t chr, const uint8_t *msg, size_t len)
{
    x->n_out = 0;
    proto_seg_tx_t tx;
    uint8_t seg[CORE_MTU_MAX];
    proto_seg_tx_init(&tx, msg, len, core_segment_max(x->core.mtu));
    size_t n;
    while ((n = proto_seg_tx_next(&tx, seg, sizeof(seg))) > 0) {
        core_write(&x->core, chr, seg, n);
    }
}

void xcore_send_json(xcore_t *x, uint8_t chr, const char *json)
{
    uint8_t buf[PROTO_LIMIT_MAX_MESSAGE_NOTIFICATION];
    size_t n = msg_from_json(json, buf, sizeof(buf));
    xcore_send(x, chr, buf, n);
}

int xcore_messages(const xcore_t *x, xcore_msg_t *out, int cap)
{
    static uint8_t bufs[6][PROTO_LIMIT_MAX_MESSAGE_NOTIFICATION];
    proto_seg_rx_t rx[6];
    for (int i = 0; i < 6; i++) {
        proto_seg_rx_init(&rx[i], bufs[i], sizeof(bufs[i]));
    }
    int n = 0;
    for (int i = 0; i < x->n_out; i++) {
        proto_seg_event_t ev[2];
        size_t k = proto_seg_rx_feed(&rx[x->out[i].chr % 6], x->out[i].v, x->out[i].len, ev);
        for (size_t j = 0; j < k && n < cap; j++) {
            if (ev[j].status == PROTO_OK) {
                out[n].chr = x->out[i].chr;
                out[n].len = ev[j].len;
                memcpy(out[n].m, ev[j].msg, ev[j].len);
                n++;
            }
        }
    }
    return n;
}

char *xcore_msg_json(const xcore_t *x, int i, uint8_t *chr)
{
    static xcore_msg_t msgs[32];
    int n = xcore_messages(x, msgs, 32);
    if (i >= n) {
        char *s = malloc(1);
        s[0] = 0;
        return s;
    }
    if (chr) {
        *chr = msgs[i].chr;
    }
    return msg_to_json_str(msgs[i].m, msgs[i].len);
}
