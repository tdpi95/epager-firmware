#include "epager/core.h"

#include <string.h>

/* Bytes around XFER_CHUNK data in one segment: segment header 1, message header 4, handle TLV 4,
 * offset TLV 7, data TLV header 3. */
#define CHUNK_OVERHEAD 19u
#define CLOCK_REBASE_MS 3600000u
/* CONFIG_CHANGED lists each key at most once: header, revision TLV, one u16 TLV per key. */
#define CHANGED_EVENT_MAX (PROTO_HEADER_LEN + 7u + 5u * PROTO_CFG_KEY_COUNT)
/* DEVICE_STATUS with every field: header, 7 u8/bool TLVs, uptime, notif_count, the SSID TLV. */
#define STATUS_MAX (PROTO_HEADER_LEN + 7u * 4u + 7u + 5u + 3u + CORE_SSID_MAX)
#define WIFI_PASSWORD_MIN 8u /* WPA2/WPA3 passphrase (protocol.md 5) */

static uint32_t now_ms(core_t *c) { return c->p.now_ms ? c->p.now_ms(c->p.ctx) : 0; }

/* ---------------------------------------------------------------- sending */

static void send(core_t *c, uint8_t chr, const uint8_t *msg, size_t len)
{
    if (!c->connected || !c->p.notify) {
        return;
    }
    proto_seg_tx_t tx;
    proto_seg_tx_init(&tx, msg, len, core_segment_max(c->mtu));
    size_t n;
    while ((n = proto_seg_tx_next(&tx, c->seg, sizeof(c->seg))) > 0) {
        c->p.notify(c->p.ctx, chr, c->seg, n);
    }
}

/* Events raised while a request is handled wait for its response (protocol.md 3). */
static void event(core_t *c, uint8_t chr, const uint8_t *msg, size_t len)
{
    if (c->in_request && c->n_ev < CORE_EVENT_MAX && len <= CORE_EVENT_ARENA - c->ev_used) {
        memcpy(c->ev_arena + c->ev_used, msg, len);
        c->ev[c->n_ev].chr = chr;
        c->ev[c->n_ev].off = (uint16_t)c->ev_used;
        c->ev[c->n_ev].len = (uint16_t)len;
        c->ev_used += len;
        c->n_ev++;
        return;
    }
    send(c, chr, msg, len); /* queue full: send now rather than lose it */
}

static void flush_events(core_t *c)
{
    for (size_t i = 0; i < c->n_ev; i++) {
        send(c, c->ev[i].chr, c->ev_arena + c->ev[i].off, c->ev[i].len);
    }
    c->n_ev = 0;
    c->ev_used = 0;
}

static void send_error(core_t *c, uint8_t chr, proto_status_t st, uint16_t req_id, int msg_type)
{
    uint8_t b[16];
    proto_writer_t w;
    proto_writer_init(&w, b, sizeof(b));
    proto_w_header(&w, PROTO_MSG_ERROR, req_id);
    proto_w_u8(&w, PROTO_TAG_ERROR_STATUS, (uint8_t)st);
    if (msg_type >= 0) {
        proto_w_u8(&w, PROTO_TAG_ERROR_MSG_TYPE, (uint8_t)msg_type);
    }
    send(c, chr, b, w.len);
}

/* ---------------------------------------------------------------- status */

static size_t write_status(core_t *c, uint8_t *buf, size_t cap)
{
    proto_writer_t w;
    proto_writer_init(&w, buf, cap);
    proto_w_header(&w, PROTO_MSG_DEVICE_STATUS, 0);
    proto_w_u8(&w, PROTO_TAG_DEVICE_STATUS_BATTERY_PCT, c->status.battery_pct);
    proto_w_u8(&w, PROTO_TAG_DEVICE_STATUS_EXTERNAL_POWER, c->status.external_power);
    proto_w_u32(&w, PROTO_TAG_DEVICE_STATUS_UPTIME_S, now_ms(c) / 1000u);
    proto_w_u16(&w, PROTO_TAG_DEVICE_STATUS_NOTIF_COUNT, (uint16_t)npipe_count(&c->np, true));
    proto_w_u8(&w, PROTO_TAG_DEVICE_STATUS_BONDED_COUNT, c->status.bonded_count);
    proto_w_bool(&w, PROTO_TAG_DEVICE_STATUS_PAIRING_MODE, c->status.pairing_mode);
    proto_w_u8(&w, PROTO_TAG_DEVICE_STATUS_OTA_STATE, c->status.ota_state);
    if (c->p.features & PROTO_FEATURES_WIFI) {
        proto_w_u8(&w, PROTO_TAG_DEVICE_STATUS_WIFI_STATE, c->status.wifi_state);
        if (c->status.wifi_ssid_len) {
            proto_w_bytes(&w, PROTO_TAG_DEVICE_STATUS_WIFI_SSID, c->status.wifi_ssid, c->status.wifi_ssid_len);
        }
    }
    if (c->p.features & PROTO_FEATURES_WEATHER) {
        proto_w_u8(&w, PROTO_TAG_DEVICE_STATUS_WEATHER_STATE, c->status.weather_state);
    }
    return proto_writer_ok(&w) ? w.len : 0;
}

static void status_changed(core_t *c)
{
    if (memcmp(&c->status, &c->sent, sizeof(c->status)) == 0) {
        return;
    }
    c->sent = c->status;
    uint8_t b[STATUS_MAX];
    size_t n = write_status(c, b, sizeof(b));
    event(c, CORE_CHR_STATUS, b, n);
}

void core_poll_battery(core_t *c)
{
    uint8_t pct = CORE_BATTERY_UNKNOWN, ext = PROTO_EXTERNAL_POWER_UNKNOWN;
    if (c->p.read_battery && !c->p.read_battery(c->p.ctx, &pct, &ext)) {
        pct = CORE_BATTERY_UNKNOWN;
    }
    c->status.battery_pct = pct > 100u ? CORE_BATTERY_UNKNOWN : pct;
    c->status.external_power = ext > PROTO_EXTERNAL_POWER_UNKNOWN ? PROTO_EXTERNAL_POWER_UNKNOWN : ext;
    c->last_battery_ms = now_ms(c);
    status_changed(c);
}

uint8_t core_battery_pct(const core_t *c) { return c->status.battery_pct; }

void core_set_pairing_mode(core_t *c, bool on)
{
    c->status.pairing_mode = on;
    status_changed(c);
}

void core_set_bonded_count(core_t *c, uint8_t n)
{
    c->status.bonded_count = n;
    status_changed(c);
}

static void set_ssid(core_t *c, const uint8_t *ssid, size_t len)
{
    len = len > CORE_SSID_MAX ? CORE_SSID_MAX : len;
    memset(c->status.wifi_ssid, 0, sizeof(c->status.wifi_ssid));
    if (len) {
        memcpy(c->status.wifi_ssid, ssid, len);
    }
    c->status.wifi_ssid_len = (uint8_t)len;
}

void core_set_wifi_status(core_t *c, uint8_t wifi_state, const char *ssid)
{
    c->status.wifi_state = wifi_state;
    set_ssid(c, (const uint8_t *)ssid, ssid ? strlen(ssid) : 0);
    status_changed(c);
}

void core_set_weather_state(core_t *c, uint8_t weather_state)
{
    c->status.weather_state = weather_state;
    status_changed(c);
}

/* ---------------------------------------------------------------- clock */

uint32_t core_now_utc(core_t *c)
{
    if (!c->clock_set) {
        return 0;
    }
    uint32_t elapsed = now_ms(c) - c->ms_base;
    if (elapsed >= CLOCK_REBASE_MS) { /* keep the millisecond difference far from wrapping */
        c->utc_base += elapsed / 1000u;
        c->ms_base += elapsed / 1000u * 1000u;
        elapsed %= 1000u;
    }
    return c->utc_base + elapsed / 1000u;
}

void core_set_utc(core_t *c, uint32_t utc_s)
{
    c->clock_set = utc_s != 0;
    c->utc_base = utc_s;
    c->ms_base = now_ms(c);
}

/* ---------------------------------------------------------------- component callbacks */

static uint32_t cb_now_utc(void *ctx) { return core_now_utc(ctx); }
uint32_t core_time_sync_age_ms(core_t *c) { return c->time_synced ? now_ms(c) - c->time_sync_ms : UINT32_MAX; }

static void cb_set_time(void *ctx, uint32_t utc_s)
{
    core_t *c = ctx;
    core_set_utc(c, utc_s);
    c->time_synced = utc_s != 0;
    c->time_sync_ms = now_ms(c);
    if (utc_s && c->p.rtc_set) {
        (void)c->p.rtc_set(c->p.ctx, utc_s); /* a failed write only costs the time at the next reset */
    }
}

/* ---------------------------------------------------------------- clock face option values (M8b)
 * Each face keeps its own option values in the file "opt_<id>" next to the assets (PLAN D113): selecting a face loads
 * them into ui.clock_face_opts, a write of that key stores them, and a face that is deleted takes its file along. */

static bool face_opt_name(const uint8_t *id, size_t len, char suffix, char out[BLOB_NAME_MAX + 1])
{
    if (len == 0 || len > XFER_ID_MAX) {
        return false;
    }
    memcpy(out, "opt_", 4);
    memcpy(out + 4, id, len);
    out[4 + len] = suffix;
    out[4 + len + (suffix ? 1 : 0)] = '\0';
    return true;
}

/* The active face's option values go to its file (an empty value removes it). */
static void face_opts_store(core_t *c)
{
    const blob_storage_t *b = c->p.blob;
    const uint8_t *id, *opts;
    char name[BLOB_NAME_MAX + 1], temp[BLOB_NAME_MAX + 1];
    const size_t id_len = cfg_get_str(&c->cfg, PROTO_CFG_UI_CLOCK_FACE_ID, &id);
    const size_t n = cfg_get_str(&c->cfg, PROTO_CFG_UI_CLOCK_FACE_OPTS, &opts);
    if (!b || !face_opt_name(id, id_len, 0, name) || !face_opt_name(id, id_len, '~', temp)) {
        return;
    }
    if (n == 0) {
        (void)b->remove(b->ctx, name);
    } else if (b->remove(b->ctx, temp) == BLOB_OK && b->write(b->ctx, temp, 0, opts, n) == BLOB_OK &&
               b->flush(b->ctx, temp) == BLOB_OK) {
        (void)b->rename(b->ctx, temp, name);
    }
}

/* ui.clock_face_id changed: the new face's stored values become the active ones (none stored: the defaults). */
static void face_opts_load(core_t *c)
{
    const blob_storage_t *b = c->p.blob;
    const uint8_t *id;
    uint8_t v[32]; /* the key's maximum length */
    uint32_t size = 0;
    char name[BLOB_NAME_MAX + 1];
    const size_t id_len = cfg_get_str(&c->cfg, PROTO_CFG_UI_CLOCK_FACE_ID, &id);
    size_t n = 0;
    if (b && face_opt_name(id, id_len, 0, name) && b->stat(b->ctx, name, &size) == BLOB_OK && size <= sizeof(v) &&
        b->read(b->ctx, name, 0, v, size) == BLOB_OK) {
        n = size;
    }
    (void)core_config_set(c, PROTO_CFG_UI_CLOCK_FACE_OPTS, v, n);
}

static void face_config_changed(core_t *c, const uint16_t *keys, size_t n_keys)
{
    bool id = false, opts = false;
    for (size_t i = 0; i < n_keys; i++) {
        id |= keys[i] == PROTO_CFG_UI_CLOCK_FACE_ID;
        opts |= keys[i] == PROTO_CFG_UI_CLOCK_FACE_OPTS;
    }
    if (opts) {
        face_opts_store(c); /* an explicit write wins over the new face's stored values */
    } else if (id) {
        face_opts_load(c);
    }
}

/* A face was installed or deleted: drop the option files of faces that are gone and, when the active face is gone,
 * go back to Classic (the face id key is cleared, the phone sees it). */
static void face_assets_changed(core_t *c)
{
    const blob_storage_t *b = c->p.blob;
    if (!b) {
        return;
    }
    char name[BLOB_NAME_MAX + 1], face[BLOB_NAME_MAX + 1];
    uint8_t v[1] = {0};
    uint32_t i = 0, size;
    while (b->list(b->ctx, i++, name, sizeof(name), &size) == BLOB_OK) {
        if (strncmp(name, "opt_", 4) != 0 || name[strlen(name) - 1] == '~') {
            continue;
        }
        xfer_asset_blob_name(PROTO_ASSET_TYPE_CLOCK_FACE, name + 4, strlen(name + 4), face);
        if (b->stat(b->ctx, face, &size) == BLOB_NOT_FOUND && b->remove(b->ctx, name) == BLOB_OK) {
            i = 0; /* the listing order may have changed */
        }
    }
    const uint8_t *id;
    const size_t id_len = cfg_get_str(&c->cfg, PROTO_CFG_UI_CLOCK_FACE_ID, &id);
    if (id_len) {
        xfer_asset_blob_name(PROTO_ASSET_TYPE_CLOCK_FACE, (const char *)id, id_len, face);
        if (b->stat(b->ctx, face, &size) == BLOB_NOT_FOUND) {
            (void)core_config_set(c, PROTO_CFG_UI_CLOCK_FACE_ID, v, 0);
        }
    }
}

static void cb_config_changed(void *ctx, uint32_t revision, const uint16_t *keys, size_t n_keys)
{
    core_t *c = ctx;
    uint8_t b[CHANGED_EVENT_MAX];
    proto_writer_t w;
    proto_writer_init(&w, b, sizeof(b));
    cfg_write_changed_event(&w, revision, keys, n_keys);
    if (proto_writer_ok(&w)) {
        event(c, CORE_CHR_CONFIG, b, w.len);
    }
    face_config_changed(c, keys, n_keys);
    if (c->p.on_config) {
        c->p.on_config(c->p.ctx, keys, n_keys);
    }
    /* committed by the phone's CONFIG_SET (not TIME_SYNC's time zone, a policy change or the board's own write) */
    if (c->in_request && c->request_type == PROTO_MSG_CONFIG_SET && c->p.on_setting) {
        c->p.on_setting(c->p.ctx, CORE_SETTING_CONFIG);
    }
}

static void cb_app_seen(void *ctx, const npipe_app_t *a)
{
    core_t *c = ctx;
    uint8_t b[256];
    proto_writer_t w;
    proto_writer_init(&w, b, sizeof(b));
    npipe_write_app_seen_event(&w, a);
    if (proto_writer_ok(&w)) {
        event(c, CORE_CHR_CONFIG, b, w.len);
    }
}

/* The icon of an app that left the seen list goes with it (protocol.md 6.4). */
static void cb_app_removed(void *ctx, const uint8_t *id, size_t len)
{
    core_t *c = ctx;
    char icon_id[XFER_APP_ICON_ID_LEN + 1];
    xfer_app_icon_id((const char *)id, len, icon_id);
    (void)xfer_asset_remove(&c->xfer, PROTO_ASSET_TYPE_APP_ICON, icon_id, XFER_APP_ICON_ID_LEN);
}

proto_status_t core_config_set(core_t *c, uint16_t key, const uint8_t *val, size_t len)
{
    const cfg_entry_t e = {.key = key, .val = val, .len = len};
    proto_status_t key_status;
    const proto_status_t st = cfg_set(&c->cfg, &e, 1, NULL, &key_status);
    return st == PROTO_OK ? key_status : st; /* an unknown key is reported per key */
}

proto_status_t core_config_set_u8(core_t *c, uint16_t key, uint8_t value) { return core_config_set(c, key, &value, 1); }

proto_status_t core_face_select(core_t *c, const char *id)
{
    return core_config_set(c, PROTO_CFG_UI_CLOCK_FACE_ID, (const uint8_t *)id, strlen(id));
}

proto_status_t core_face_option(core_t *c, uint8_t option, uint8_t value)
{
    const uint8_t *old;
    uint8_t pairs[32];
    size_t n = 0, old_len = cfg_get_str(&c->cfg, PROTO_CFG_UI_CLOCK_FACE_OPTS, &old);
    for (size_t i = 0; i + 1 < old_len && n + 2 <= sizeof(pairs) - 2; i += 2) { /* keep the others, room for this one */
        if (old[i] != option) {
            pairs[n++] = old[i];
            pairs[n++] = old[i + 1];
        }
    }
    pairs[n++] = option;
    pairs[n++] = value;
    return core_config_set(c, PROTO_CFG_UI_CLOCK_FACE_OPTS, pairs, n);
}

proto_status_t core_face_delete(core_t *c, const char *id)
{
    return xfer_asset_remove(&c->xfer, PROTO_ASSET_TYPE_CLOCK_FACE, id, strlen(id));
}

bool core_dismiss(core_t *c, uint32_t id)
{
    npipe_dismissed_t d;
    if (npipe_dismiss(&c->np, id, &d) != PROTO_OK) {
        return false;
    }
    uint8_t b[PROTO_HEADER_LEN + 6u + 3u + 2u + NPIPE_KEY_MAX];
    proto_writer_t w;
    proto_writer_init(&w, b, sizeof(b));
    npipe_write_dismissed_event(&w, &d);
    if (proto_writer_ok(&w)) {
        event(c, CORE_CHR_NOTIFICATION, b, w.len);
    }
    if (c->p.on_dismissed) {
        c->p.on_dismissed(c->p.ctx, &d);
    }
    return true;
}

static void cb_notif(void *ctx, const npipe_notif_t *n)
{
    core_t *c = ctx;
    if (c->p.on_notif) {
        c->p.on_notif(c->p.ctx, n);
    }
}

static bool cb_renderable(uint32_t cp, void *ctx)
{
    core_t *c = ctx;
    return c->p.renderable(cp, c->p.ctx);
}

static void cb_asset(void *ctx, uint8_t asset_type)
{
    core_t *c = ctx;
    if (asset_type == PROTO_ASSET_TYPE_CLOCK_FACE) {
        face_assets_changed(c);
    }
    if (c->p.on_asset) {
        c->p.on_asset(c->p.ctx, asset_type);
    }
}

static proto_status_t cb_check_asset(void *ctx, uint8_t asset_type, const char *id, size_t id_len, const char *blob_name,
                                     uint32_t size)
{
    core_t *c = ctx;
    return c->p.check_asset ? c->p.check_asset(c->p.ctx, asset_type, id, id_len, blob_name, size) : PROTO_OK;
}

static void cb_xfer_emit(void *ctx, const uint8_t *msg, size_t len) { event(ctx, CORE_CHR_TRANSFER, msg, len); }

static bool cb_read_battery(void *ctx, uint8_t *pct, uint8_t *ext)
{
    core_t *c = ctx;
    return c->p.read_battery(c->p.ctx, pct, ext);
}

static void cb_ota(void *ctx, const xfer_ota_event_t *ev)
{
    core_t *c = ctx;
    if (ev->kind == XFER_OTA_EV_STATE) {
        c->status.ota_state = ev->state;
        status_changed(c);
    }
    if (c->p.on_ota) {
        c->p.on_ota(c->p.ctx, ev);
    }
}

/* ---------------------------------------------------------------- lifecycle */

size_t core_segment_max(uint16_t mtu)
{
    size_t n = mtu > 3u ? mtu - 3u : 0u;
    return n > CORE_SEGMENT_MAX ? CORE_SEGMENT_MAX : n;
}

uint16_t core_max_chunk(uint16_t mtu)
{
    size_t seg = core_segment_max(mtu);
    size_t n = seg > CHUNK_OVERHEAD ? seg - CHUNK_OVERHEAD : 1u;
    return (uint16_t)(n > PROTO_LIMIT_CHUNK_DATA_MAX ? PROTO_LIMIT_CHUNK_DATA_MAX : n);
}

static uint16_t clamp_mtu(uint16_t mtu)
{
    return mtu < CORE_MTU_DEFAULT ? CORE_MTU_DEFAULT : mtu > CORE_MTU_MAX ? CORE_MTU_MAX : mtu;
}

static void reset_rx(core_t *c)
{
    proto_seg_rx_init(&c->rx_config, c->rx_config_buf, sizeof(c->rx_config_buf));
    proto_seg_rx_init(&c->rx_notif, c->rx_notif_buf, sizeof(c->rx_notif_buf));
    proto_seg_rx_init(&c->rx_xfer, c->rx_xfer_buf, sizeof(c->rx_xfer_buf));
}

void core_init(core_t *c, const core_params_t *p)
{
    memset(c, 0, sizeof(*c));
    c->p = *p;
    c->mtu = CORE_MTU_DEFAULT;
    reset_rx(c);

    cfg_params_t cp = {.kv = p->kv_cfg, .variant = p->variant, .on_change = cb_config_changed,
                       .set_time = cb_set_time, .ctx = c};
    cfg_init(&c->cfg, &cp);

    npipe_params_t np = {.cfg = &c->cfg, .kv = p->kv_apps, .slots = p->slots, .n_slots = p->n_slots,
                         .apps = p->apps, .n_apps = p->n_apps, .now_utc = cb_now_utc,
                         .renderable = p->renderable ? cb_renderable : NULL,
                         .on_event = cb_notif, .on_app_seen = cb_app_seen, .on_app_removed = cb_app_removed,
                         .ctx = c};
    npipe_init(&c->np, &np);

    if (p->fw) {
        xfer_ota_params_t op = {.running_version = p->firmware_version, .allow_downgrade = p->allow_downgrade,
                                .low_battery_pct = p->low_battery_pct,
                                .read_battery = p->read_battery ? cb_read_battery : NULL, .kv = p->kv_xfer,
                                .required_checks = p->ota_required_checks,
                                .verify_timeout_ms = p->ota_verify_timeout_ms, .on_event = cb_ota, .ctx = c};
        xfer_ota_init(&c->ota, &op);
    }
    xfer_params_t xp = {.kv = p->kv_xfer, .blob = p->blob, .assets = p->assets, .n_assets = p->n_assets,
                        .display_w = p->display_w, .display_h = p->display_h, .fw = p->fw,
                        .ota = p->fw ? &c->ota : NULL, .max_chunk = core_max_chunk(c->mtu),
                        .ack_window = p->ack_window, .journal_every = p->journal_every, .emit = cb_xfer_emit,
                        .on_asset = cb_asset, .check_asset = cb_check_asset, .ctx = c};
    xfer_init(&c->xfer, &xp);

    uint32_t rtc = 0;
    if (p->rtc_get && p->rtc_get(p->ctx, &rtc) && rtc) {
        core_set_utc(c, rtc);
    }
    c->status.ota_state = PROTO_OTA_STATE_NONE;
    core_poll_battery(c);
    c->sent = c->status;
    c->last_flush_ms = now_ms(c);
}

void core_boot(core_t *c, bool image_pending_verify)
{
    if (c->p.fw) {
        xfer_ota_boot(&c->ota, image_pending_verify, now_ms(c));
    }
}

void core_set_mtu(core_t *c, uint16_t mtu)
{
    c->mtu = clamp_mtu(mtu);
    c->xfer.p.max_chunk = core_max_chunk(c->mtu);
}

void core_connect(core_t *c, uint16_t mtu, bool secure)
{
    reset_rx(c);
    c->connected = true;
    c->secure = secure;
    c->n_ev = c->ev_used = 0;
    core_set_mtu(c, mtu);
}

void core_set_secure(core_t *c, bool secure) { c->secure = secure; }

void core_disconnect(core_t *c)
{
    if (!c->connected) {
        return;
    }
    c->connected = false;
    xfer_on_disconnect(&c->xfer);
    npipe_apps_flush(&c->np);
    c->last_flush_ms = now_ms(c);
    reset_rx(c);
}

void core_tick(core_t *c)
{
    uint32_t t = now_ms(c);
    if (c->p.fw) {
        xfer_ota_tick(&c->ota, t);
    }
    if (c->p.battery_poll_ms && t - c->last_battery_ms >= c->p.battery_poll_ms) {
        core_poll_battery(c);
    }
    if (c->p.apps_flush_ms && t - c->last_flush_ms >= c->p.apps_flush_ms) {
        npipe_apps_flush(&c->np);
        c->last_flush_ms = t;
    }
    (void)core_now_utc(c); /* rebase the soft clock */
}

/* ---------------------------------------------------------------- reads */

static size_t write_capabilities(core_t *c, uint8_t *buf, size_t cap)
{
    const core_params_t *p = &c->p;
    proto_writer_t w;
    proto_writer_init(&w, buf, cap);
    proto_w_header(&w, PROTO_MSG_CAPABILITIES, 0);
    proto_w_str(&w, PROTO_TAG_CAPABILITIES_CONTRACT_VERSION, PROTO_CONTRACT_VERSION);
    proto_w_str(&w, PROTO_TAG_CAPABILITIES_DEVICE_NAME, p->device_name);
    proto_w_str(&w, PROTO_TAG_CAPABILITIES_BOARD_ID, p->board_id);
    proto_w_u8(&w, PROTO_TAG_CAPABILITIES_VARIANT, p->variant);
    proto_w_str(&w, PROTO_TAG_CAPABILITIES_FIRMWARE_VERSION, p->firmware_version);
    proto_w_struct_begin(&w, PROTO_TAG_CAPABILITIES_DISPLAY);
    proto_w_u8(&w, PROTO_TAG_DISPLAY_INFO_TYPE, p->display_type);
    proto_w_u16(&w, PROTO_TAG_DISPLAY_INFO_WIDTH, p->display_w);
    proto_w_u16(&w, PROTO_TAG_DISPLAY_INFO_HEIGHT, p->display_h);
    proto_w_u8(&w, PROTO_TAG_DISPLAY_INFO_COLOR_DEPTH, p->color_depth);
    proto_w_u8(&w, PROTO_TAG_DISPLAY_INFO_REFRESH_CLASS, p->refresh_class);
    proto_w_struct_end(&w);
    proto_w_u32(&w, PROTO_TAG_CAPABILITIES_FEATURES, p->features);
    proto_w_u8(&w, PROTO_TAG_CAPABILITIES_TRANSPORTS, p->transports);
    uint32_t total, free_bytes;
    if (p->blob && p->blob->usage(p->blob->ctx, &total, &free_bytes) == BLOB_OK) {
        proto_w_u32(&w, PROTO_TAG_CAPABILITIES_STORAGE_TOTAL, total);
        proto_w_u32(&w, PROTO_TAG_CAPABILITIES_STORAGE_FREE, free_bytes);
    }
    for (size_t i = 0; p->blob && i < p->n_assets; i++) {
        proto_w_struct_begin(&w, PROTO_TAG_CAPABILITIES_ASSET_SUPPORT);
        proto_w_u8(&w, PROTO_TAG_ASSET_SUPPORT_ASSET_TYPE, p->assets[i].asset_type);
        proto_w_u8(&w, PROTO_TAG_ASSET_SUPPORT_FORMAT, p->assets[i].format);
        proto_w_u32(&w, PROTO_TAG_ASSET_SUPPORT_MAX_SIZE, p->assets[i].max_size);
        if (p->assets[i].width || p->assets[i].height) {
            proto_w_u16(&w, PROTO_TAG_ASSET_SUPPORT_WIDTH, p->assets[i].width);
            proto_w_u16(&w, PROTO_TAG_ASSET_SUPPORT_HEIGHT, p->assets[i].height);
        }
        proto_w_struct_end(&w);
    }
    proto_w_u16(&w, PROTO_TAG_CAPABILITIES_MAX_MTU, p->max_mtu);
    proto_w_u32(&w, PROTO_TAG_CAPABILITIES_CONFIG_REVISION, cfg_revision(&c->cfg));
    proto_w_bool(&w, PROTO_TAG_CAPABILITIES_PAIRING_MODE, c->status.pairing_mode);
    return proto_writer_ok(&w) && w.len <= PROTO_LIMIT_CAPABILITIES_MAX ? w.len : 0;
}

size_t core_read(core_t *c, uint8_t chr, uint8_t *out, size_t cap)
{
    if (chr == CORE_CHR_CAPABILITIES) {
        return write_capabilities(c, out, cap);
    }
    if (chr == CORE_CHR_STATUS && c->secure) {
        return write_status(c, out, cap);
    }
    return 0;
}

/* ---------------------------------------------------------------- writes */

static proto_seg_rx_t *rx_for(core_t *c, uint8_t chr)
{
    switch (chr) {
    case CORE_CHR_CONFIG: return &c->rx_config;
    case CORE_CHR_NOTIFICATION: return &c->rx_notif;
    case CORE_CHR_TRANSFER: return &c->rx_xfer;
    default: return NULL;
    }
}

static uint8_t channel_of(uint8_t chr)
{
    return chr == CORE_CHR_CONFIG ? PROTO_CHAN_CONFIG
         : chr == CORE_CHR_NOTIFICATION ? PROTO_CHAN_NOTIFICATION : PROTO_CHAN_TRANSFER;
}

static void send_ack(core_t *c, uint8_t handle, proto_status_t st)
{
    uint8_t b[24];
    proto_writer_t w;
    proto_writer_init(&w, b, sizeof(b));
    proto_w_header(&w, PROTO_MSG_XFER_ACK, 0);
    proto_w_u8(&w, PROTO_TAG_XFER_ACK_HANDLE, handle);
    proto_w_u32(&w, PROTO_TAG_XFER_ACK_OFFSET, 0);
    proto_w_u8(&w, PROTO_TAG_XFER_ACK_STATUS, (uint8_t)st);
    send(c, CORE_CHR_TRANSFER, b, w.len);
}

static void handle_ping(const proto_msg_t *m, proto_writer_t *w)
{
    proto_w_response(w, m, PROTO_OK);
    const uint8_t *v;
    uint16_t n;
    if (proto_find(m->body, m->body_len, PROTO_TAG_PING_PAYLOAD, &v, &n)) {
        proto_w_bytes(w, PROTO_TAG_PING_RSP_PAYLOAD, v, n);
    }
}

/* WIFI_SET (protocol.md 5): feature, forget, ssid, password, in that order. */
static void handle_wifi_set(core_t *c, const proto_msg_t *m, proto_writer_t *w)
{
    const uint8_t *ssid = NULL, *pw = NULL;
    uint16_t ssid_len = 0, pw_len = 0;
    bool forget = false;
    (void)proto_get_bool(m->body, m->body_len, PROTO_TAG_WIFI_SET_FORGET, &forget);
    bool has_ssid = proto_find(m->body, m->body_len, PROTO_TAG_WIFI_SET_SSID, &ssid, &ssid_len);
    (void)proto_find(m->body, m->body_len, PROTO_TAG_WIFI_SET_PASSWORD, &pw, &pw_len);

    proto_status_t st;
    if (!(c->p.features & PROTO_FEATURES_WIFI) || !c->p.wifi_set) {
        st = PROTO_UNSUPPORTED;
    } else if (forget) {
        st = c->p.wifi_set(c->p.ctx, has_ssid ? ssid : NULL, has_ssid ? ssid_len : 0, NULL, 0, true);
    } else if (!has_ssid) {
        st = PROTO_MISSING_FIELD;
    } else if (pw_len > 0 && pw_len < WIFI_PASSWORD_MIN) {
        st = PROTO_OUT_OF_RANGE;
    } else {
        st = c->p.wifi_set(c->p.ctx, ssid, ssid_len, pw_len ? pw : NULL, pw_len, false);
    }
    proto_w_response(w, m, st);
    if (st == PROTO_OK) {
        /* The radio task later replaces this with the selected/last-connected SSID. */
        if (forget && !has_ssid) {
            c->status.wifi_state = PROTO_WIFI_STATE_UNCONFIGURED;
            set_ssid(c, NULL, 0);
        } else if (!forget) {
            c->status.wifi_state = PROTO_WIFI_STATE_IDLE;
            set_ssid(c, ssid, ssid_len);
        }
        status_changed(c); /* an event: sent after the response */
        if (c->p.on_setting) {
            c->p.on_setting(c->p.ctx, CORE_SETTING_WIFI);
        }
    }
}

static void handle_message(core_t *c, uint8_t chr, const uint8_t *buf, size_t len)
{
    if (len < PROTO_HEADER_LEN) {
        send_error(c, chr, PROTO_MALFORMED, 0, len >= 2 ? buf[1] : -1);
        return;
    }
    uint8_t type = buf[1];
    uint16_t req_id = proto_rd16(buf + 2);
    const proto_msg_desc_t *d = proto_msg_desc(type);
    if (buf[0] != PROTO_MAJOR) {
        send_error(c, chr, PROTO_UNSUPPORTED_VERSION, req_id, type);
        return;
    }
    if (!d || d->kind != PROTO_KIND_REQUEST || (d->channel != PROTO_CHAN_ANY && d->channel != channel_of(chr))) {
        send_error(c, chr, PROTO_UNKNOWN_MESSAGE, req_id, type);
        return;
    }

    size_t cap = chr == CORE_CHR_CONFIG ? PROTO_LIMIT_MAX_MESSAGE_CONFIG
               : chr == CORE_CHR_NOTIFICATION ? PROTO_LIMIT_MAX_MESSAGE_NOTIFICATION
                                              : PROTO_LIMIT_MAX_MESSAGE_TRANSFER;
    proto_writer_t w;
    proto_writer_init(&w, c->out, cap);
    proto_msg_t m;
    proto_status_t st = proto_msg_parse(buf, len, &m);
    if (!c->secure) {
        if (type == PROTO_MSG_XFER_CHUNK) {
            send_ack(c, 0, PROTO_INSUFFICIENT_SECURITY);
            return;
        }
        proto_w_response(&w, &m, PROTO_INSUFFICIENT_SECURITY);
    } else if (st != PROTO_OK) {
        if (type == PROTO_MSG_XFER_CHUNK) {
            c->in_request = true;
            xfer_chunk_invalid(&c->xfer, &m, st);
        } else {
            proto_w_response(&w, &m, st);
        }
    } else {
        c->in_request = true;
        c->request_type = type;
        if (type == PROTO_MSG_PING) {
            handle_ping(&m, &w);
        } else if (type == PROTO_MSG_WIFI_SET) {
            handle_wifi_set(c, &m, &w);
        } else if (chr == CORE_CHR_CONFIG) {
            if (!cfg_handle(&c->cfg, &m, &w)) {
                (void)npipe_handle(&c->np, &m, &w);
            }
        } else if (chr == CORE_CHR_NOTIFICATION) {
            (void)npipe_handle(&c->np, &m, &w);
        } else {
            (void)xfer_handle(&c->xfer, &m, &w);
        }
    }
    c->in_request = false;
    if (w.len && !proto_writer_ok(&w)) { /* handlers size their answers to the channel; never expected */
        proto_writer_init(&w, c->out, cap);
        proto_w_response(&w, &m, PROTO_INTERNAL);
    }
    if (w.len) {
        send(c, chr, c->out, w.len);
    }
    flush_events(c);
}

void core_write(core_t *c, uint8_t chr, const uint8_t *val, size_t len)
{
    proto_seg_rx_t *rx = rx_for(c, chr);
    if (!rx) {
        return;
    }
    proto_seg_event_t ev[2];
    size_t n = proto_seg_rx_feed(rx, val, len, ev);
    for (size_t i = 0; i < n; i++) {
        if (ev[i].status != PROTO_OK) {
            send_error(c, chr, ev[i].status, 0, -1);
        } else {
            handle_message(c, chr, ev[i].msg, ev[i].len);
        }
    }
}
