/*
 * Board core: the one object the core task owns. Routes characteristic writes to the config
 * store, notification pipeline and transfer state machine (protocol.md 2.1 and 2.4: reassembly,
 * header checks, routing, security gate, validation), segments answers at the link MTU, sends
 * events (CONFIG_CHANGED and APP_SEEN after the response that caused them, XFER_ACK,
 * DEVICE_STATUS on change), builds the Capabilities and Status read values, and keeps a soft UTC
 * clock. Platform access only through the interfaces in core_params_t. Portable C11;
 * single-threaded, no logging (notification content never leaves except through on_notif).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/cfg_store.h"
#include "epager/npipe.h"
#include "epager/proto.h"
#include "epager/xfer.h"
#include "epager/xfer_ota.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Characteristics of the ePager service, numbered as in the TCP transport (protocol.md 10). */
typedef enum {
    CORE_CHR_CAPABILITIES = 1,
    CORE_CHR_CONFIG = 2,
    CORE_CHR_NOTIFICATION = 3,
    CORE_CHR_TRANSFER = 4,
    CORE_CHR_STATUS = 5,
} core_chr_t;

/* What the phone changed on the board (core_params_t.on_setting). */
typedef enum { CORE_SETTING_CONFIG = 0, CORE_SETTING_WIFI } core_setting_t;

#define CORE_MTU_DEFAULT 23u
#define CORE_MTU_MAX 517u
#define CORE_SEGMENT_MAX 512u /* ATT attribute value limit (protocol.md 2.1) */
#define CORE_BATTERY_UNKNOWN 255u
#define CORE_EVENT_ARENA 1024u
#define CORE_EVENT_MAX 4u

typedef struct {
    /* identity (CAPABILITIES) */
    const char *device_name;      /* "ePager-XXXX" */
    const char *board_id;
    const char *firmware_version; /* also the running version for OTA checks */
    uint8_t variant;              /* PROTO_VARIANT_* */
    uint8_t display_type, color_depth, refresh_class;
    uint16_t display_w, display_h;
    uint32_t features;            /* PROTO_FEATURES_* */
    uint8_t transports;           /* PROTO_TRANSPORTS_* */
    uint16_t max_mtu;             /* preferred ATT MTU hint */

    /* storage; the kv instances may be the same object (their keys do not collide) */
    const kv_storage_t *kv_cfg, *kv_apps, *kv_xfer;
    const blob_storage_t *blob;   /* NULL: no assets */
    const xfer_asset_support_t *assets;
    size_t n_assets;
    const xfer_fw_sink_t *fw;     /* NULL: no firmware updates */
    bool allow_downgrade;
    uint8_t low_battery_pct;      /* OTA gate */
    uint32_t ota_required_checks, ota_verify_timeout_ms;
    uint32_t ack_window, journal_every;

    /* notification store and seen apps (caller-owned, e.g. PSRAM) */
    npipe_slot_t *slots;
    size_t n_slots;
    npipe_app_t *apps;
    size_t n_apps;
    proto_renderable_fn renderable; /* font coverage (called with ctx); NULL = reference set */

    /* platform */
    uint32_t (*now_ms)(void *ctx);  /* monotonic milliseconds (wraps) */
    /* NULL: no battery reader. False if the level cannot be read. external: PROTO_EXTERNAL_POWER_*. */
    bool (*read_battery)(void *ctx, uint8_t *pct, uint8_t *external);
    uint32_t battery_poll_ms;       /* 0 = only at init and core_poll_battery() */
    uint32_t apps_flush_ms;         /* seen-apps recency flush interval; 0 = only at disconnect */
    /* NULL: no RTC. rtc_get sets the soft clock at core_init (false or 0: the RTC holds no valid
     * time); TIME_SYNC writes the new time with rtc_set. Same signatures as board_profile_t. */
    bool (*rtc_get)(void *ctx, uint32_t *utc_s);
    bool (*rtc_set)(void *ctx, uint32_t utc_s);
    /* Boards with the wifi feature: add/update the WIFI_SET network, or forget one (forget + ssid) or
     * all (forget without ssid). Lengths are already checked (protocol.md 5); the password is write-only. Returns PROTO_OK, or an error
     * status for the response (e.g. PROTO_INTERNAL if it cannot be stored). */
    proto_status_t (*wifi_set)(void *ctx, const uint8_t *ssid, size_t ssid_len, const uint8_t *pw, size_t pw_len,
                               bool forget);

    /* outputs */
    void (*notify)(void *ctx, uint8_t chr, const uint8_t *val, size_t len); /* one notification value */
    void (*on_notif)(void *ctx, const npipe_notif_t *n);      /* shown notification added/modified/removed */
    void (*on_config)(void *ctx, const uint16_t *keys, size_t n_keys); /* committed config change */
    /* The phone changed a setting: a CONFIG_SET that committed, or a WIFI_SET that stored a network (not
     * called for the board's own config writes). The UI may say so. NULL: nothing to do. */
    void (*on_setting)(void *ctx, core_setting_t kind);
    void (*on_ota)(void *ctx, const xfer_ota_event_t *ev);   /* reboot, mark valid, rollback, progress */
    void (*on_asset)(void *ctx, uint8_t asset_type);         /* asset installed or deleted (the UI re-reads it) */
    /* Content check of an asset at XFER_FINISH (xfer_params_t.check_asset): clock faces are parsed here. NULL: none. */
    proto_status_t (*check_asset)(void *ctx, uint8_t asset_type, const char *id, size_t id_len, const char *blob_name,
                                  uint32_t size);
    /* The user dismissed a notification on the board (core_dismiss), already removed from the store. The
     * platform tells the phone: ANCS needs the negative action (protocol.md 6.5). NULL: nothing to do. */
    void (*on_dismissed)(void *ctx, const npipe_dismissed_t *d);
    void *ctx;
} core_params_t;

#define CORE_SSID_MAX 32u

typedef struct {
    uint8_t battery_pct, external_power, bonded_count, ota_state;
    bool pairing_mode;
    uint8_t wifi_state, weather_state;     /* PROTO_WIFI_STATE_*, PROTO_WEATHER_STATE_* */
    uint8_t wifi_ssid_len;                 /* 0: no network stored */
    uint8_t wifi_ssid[CORE_SSID_MAX];      /* zero-padded, so status changes compare with memcmp */
} core_status_t;

typedef struct {
    core_params_t p;
    cfg_store_t cfg;
    npipe_t np;
    xfer_t xfer;
    xfer_ota_t ota;

    bool connected, secure;
    uint16_t mtu;
    proto_seg_rx_t rx_config, rx_notif, rx_xfer;
    uint8_t rx_config_buf[PROTO_LIMIT_MAX_MESSAGE_CONFIG];
    uint8_t rx_notif_buf[PROTO_LIMIT_MAX_MESSAGE_NOTIFICATION];
    uint8_t rx_xfer_buf[PROTO_LIMIT_MAX_MESSAGE_TRANSFER];
    uint8_t out[PROTO_LIMIT_MAX_MESSAGE_NOTIFICATION]; /* the response being built */
    uint8_t seg[CORE_SEGMENT_MAX];

    /* events raised while a request is handled, sent after its response */
    bool in_request;
    uint8_t request_type;          /* the PROTO_MSG_* being handled while in_request */
    uint8_t ev_arena[CORE_EVENT_ARENA];
    size_t ev_used, n_ev;
    struct { uint8_t chr; uint16_t off, len; } ev[CORE_EVENT_MAX];

    core_status_t status, sent;
    bool clock_set, time_synced;
    uint32_t utc_base, ms_base, time_sync_ms;
    uint32_t last_battery_ms, last_flush_ms;
} core_t;

/* Load config, seen apps and the transfer journal; first battery read. Call core_boot() next. */
void core_init(core_t *c, const core_params_t *p);
/* OTA boot state. image_pending_verify: the bootloader started a new image not yet marked valid. */
void core_boot(core_t *c, bool image_pending_verify);

void core_connect(core_t *c, uint16_t mtu, bool secure);
void core_set_mtu(core_t *c, uint16_t mtu);
/* BLE links are secure by construction (ATT permissions); other transports may say otherwise. */
void core_set_secure(core_t *c, bool secure);
void core_disconnect(core_t *c);

/* One written value of a characteristic (segment header included). Writes to Capabilities and
 * Status are ignored (the ATT layer refuses them on BLE). */
void core_write(core_t *c, uint8_t chr, const uint8_t *val, size_t len);
/* Read value of Capabilities or Status (no segment header). Returns its length, 0 if the
 * characteristic is not readable or (Status) the link is not secure. cap >= PROTO_LIMIT_CAPABILITIES_MAX. */
size_t core_read(core_t *c, uint8_t chr, uint8_t *out, size_t cap);

/* Timers: OTA verify timeout, battery poll, seen-apps flush. Call every second or so. */
void core_tick(core_t *c);
void core_poll_battery(core_t *c);
void core_set_pairing_mode(core_t *c, bool on);
void core_set_bonded_count(core_t *c, uint8_t n);
/* Wi-Fi and weather states for DEVICE_STATUS (sent only with the wifi / weather features). ssid NULL or
 * empty = no network stored. A WIFI_SET that succeeds sets the name and `idle` (or `unconfigured` when
 * forgotten) by itself; the platform reports what happens next. */
void core_set_wifi_status(core_t *c, uint8_t wifi_state, const char *ssid);
void core_set_weather_state(core_t *c, uint8_t weather_state);
/* Last battery reading: CORE_BATTERY_UNKNOWN without a reader or when it failed. */
uint8_t core_battery_pct(const core_t *c);

/* The user dismissed notification `id` on the board (protocol.md 6.5): removed from the store and
 * NOTIF_DISMISSED is notified to a connected app, and on_dismissed runs (ANCS: the negative action to iOS).
 * False if the board does not hold it. */
bool core_dismiss(core_t *c, uint32_t id);
/* A config key changed on the board itself (brightness from a key, the city from the Wi-Fi setup page,
 * the time zone from the weather): CONFIG_SET rules, so the app gets CONFIG_CHANGED. `val` is the wire
 * encoding (little-endian integers, UTF-8 strings). Returns the cfg_set status. */
proto_status_t core_config_set(core_t *c, uint16_t key, const uint8_t *val, size_t len);
proto_status_t core_config_set_u8(core_t *c, uint16_t key, uint8_t value);
/* The settings pages' clock face actions (M8b): `id` ("" = Classic) becomes ui.clock_face_id, which loads that face's stored
 * option values; `value` of option `option` is put into ui.clock_face_opts and stored with the active face; delete an
 * installed face (its option values go with it, and Classic shows if it was the active one). */
proto_status_t core_face_select(core_t *c, const char *id);
proto_status_t core_face_option(core_t *c, uint8_t option, uint8_t value);
proto_status_t core_face_delete(core_t *c, const char *id);

/* Milliseconds since a phone last set the clock with TIME_SYNC; UINT32_MAX if none did since boot. The
 * board's own time sources (SNTP, PLAN D63) give way to a phone for a day. */
uint32_t core_time_sync_age_ms(core_t *c);
/* Soft UTC clock, set by TIME_SYNC or from the RTC at core_init. 0 = not set. core_set_utc does not
 * write the RTC (TIME_SYNC does). */
uint32_t core_now_utc(core_t *c);
void core_set_utc(core_t *c, uint32_t utc_s);

/* Bytes per write at this MTU: min(mtu - 3, 512) (protocol.md 2.1). */
size_t core_segment_max(uint16_t mtu);
/* Largest XFER_CHUNK data that fits one write at this MTU (chunks are single-segment). */
uint16_t core_max_chunk(uint16_t mtu);

#ifdef __cplusplus
}
#endif
