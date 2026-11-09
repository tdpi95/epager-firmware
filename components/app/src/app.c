/*
 * Firmware glue shared by the variants (PLAN D54; was the Paper's main.c in M4). Boots, takes the
 * power latch, brings up the pieces the later milestones build on, and logs what it did.
 * Notification content is never logged.
 *
 * What is real: board init, power management, LittleFS mount of the assets partition, config and seen
 * apps in NVS, the BLE service and ANCS client (components/ble_core, ancs_client; PLAN D67, D68), the
 * portable core wired to the board's battery reader and RTC, the LVGL UI on the board's display (e-paper frames or LCD areas), keys and touch actions
 * (pairing mode, dismiss, brightness), the debug console (D57), the battery log (CONFIG_EPAGER_BATTERY_LOG,
 * hw-checklist 27) and, with CONFIG_EPAGER_FEATURE_WIFI, the
 * Wi-Fi task for the weather, SNTP and the setup hotspot (components/net, D60-D63).
 * Assets: app icons are stored on the LittleFS partition through blob_fs and shown by the UI (M6, asset half).
 * Firmware OTA (CONFIG_EPAGER_FEATURE_OTA, M6): the transfer layer writes into the inactive slot (components/ota),
 * a new image runs a self-check before it is marked valid (OTA_CHECK_*), and a failed or timed-out check rolls back.
 * Core, UI and the BLE
 * event handling share this task; the NimBLE host task only queues events for it (PLAN D67), and
 * everything wakes it with a task notification.
 */
#include "epager/app.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "app_priv.h"
#include "epager/ancs_sm.h"
#include "epager/ble_core.h"
#include "epager/blob_fs.h"
#include "epager/board_api.h"
#include "epager/board_util.h"
#include "epager/core.h"
#include "epager/kv_storage.h"
#include "epager/power.h"
#include "epager/ui.h"
#include "epager/ui_features.h"
#if CONFIG_EPAGER_FEATURE_WIFI
#include "epager/net.h"
#endif
#if CONFIG_EPAGER_FEATURE_OTA
#include "epager/ota.h"
#endif
#if CONFIG_EPAGER_FEATURE_WIFI_OTA
#include "epager/net_update.h"
#include "epager/upd.h"
#endif
#if CONFIG_EPAGER_FEATURE_WEATHER
#define WEATHER_ON 1
#else
#define WEATHER_ON 0
#endif

#define TAG "epager"

#define ASSETS_LABEL "assets"
#define LOOP_BUSY_MS 20  /* UI work pending (a refresh in flight, an animation, a finger): poll fast */
#define LOOP_IDLE_MS 100
#define LOOP_PANEL_MS 250 /* asleep through a refresh: BUSY wakes the loop; this only bounds a missed wake-up */
#define CORE_TICK_MS 1000
#define PAIRING_TIMEOUT_MS (3u * 60u * 1000u) /* pairing mode closes by itself (PLAN D76) */
#define CLIMATE_POLL_MS 60000 /* one sensor measurement a minute, like the clock's redraw */
#define REARM_MS 300          /* idle: after a key woke the chip, how long until it may sleep again */
#define EV_WAKE ((board_button_t)0xFF) /* queue entry from the key wake-up interrupt, not a key press */
#define PHONE_TIME_MS (24u * 3600u * 1000u) /* a phone's TIME_SYNC wins over SNTP and the weather's zone (D63) */

/* A new firmware image is valid once all of these hold (xfer_ota, PLAN D11). Wi-Fi and the weather are not
 * part of it: a phone that cannot reach a network must still be able to keep the update. */
#define OTA_CHECK_UI 0x1u      /* storage, core and UI came up */
#define OTA_CHECK_BLE 0x2u     /* the BLE host is advertising: the next update can still arrive */
#define OTA_CHECK_RUNNING 0x4u /* the main loop has run for OTA_RUNNING_MS without a reset */
#define OTA_REQUIRED (OTA_CHECK_UI | OTA_CHECK_BLE | OTA_CHECK_RUNNING)
#define OTA_RUNNING_MS 30000u
#define OTA_VERIFY_TIMEOUT_MS 120000u /* not valid by then: roll back */
#define OTA_REBOOT_DELAY_MS 1000u     /* the XFER_FINISH answer leaves first */
#define OTA_PROGRESS_STEP 5           /* the screen shows whole steps: a panel refresh per percent is too many */
#define OTA_PROGRESS_STALE_MS 15000u  /* no chunk for this long: the transfer is over */

typedef struct {
    board_button_t btn;
    board_press_t press;
} key_ev_t;

static const app_config_t *app;
static const board_profile_t *board;
/* The large state lives in PSRAM where the image allows it: internal RAM is for Wi-Fi, BLE and LVGL. */
static kv_nvs_t kv_nvs;             /* config, seen apps and the transfer journal (PLAN D26) */
static EXT_RAM_BSS_ATTR kv_mem_t kv_ram; /* only if NVS cannot be opened: nothing survives a reset then */
static EXT_RAM_BSS_ATTR core_t core;
static EXT_RAM_BSS_ATTR ancs_sm_t ancs;
static core_params_t params;
static blob_fs_t blob_fs;                      /* the assets partition as blob_storage (transfer, UI icon lookup) */
static xfer_asset_support_t asset_support[2]; /* app icons, and clock faces (M8b); the other asset types have no reader yet */
#if CONFIG_EPAGER_FEATURE_CLOCK_FACES
static uint16_t face_w, face_h; /* the screen a clock face must be made for */
static bool face_colour;
#endif
static char firmware_version[32];
static QueueHandle_t keys;
static TaskHandle_t main_task;   /* the BLE host task, keys and the wake-up interrupt notify it */
static blec_event_t ble_ev;
static bool ble_ok, ble_secure, has_passkey;
static uint32_t passkey;
#if CONFIG_EPAGER_FEATURE_WIFI_OTA
static upd_policy_t upd;           /* the daily check and what the person chose (components/ota, upd_policy) */
static const kv_storage_t *upd_kv; /* where its record lives (the core's store) */
static bool upd_ready;
static bool upd_check_inflight;    /* a check went to the Wi-Fi task and its answer is not in yet */
static bool upd_installed_seen;
static ui_update_error_t upd_local_err; /* a refusal that never reached the Wi-Fi task: the battery, the version */
static uint32_t upd_seq_seen;
static uint8_t upd_net_state_seen;
static bool ota_wifi;              /* the image comes over Wi-Fi: its progress does not depend on the phone link */
#endif
#if CONFIG_EPAGER_FEATURE_OTA
static ota_sink_t ota_sink;
static int16_t ota_pct = -1;  /* firmware bytes received so far, -1 = no update running */
static uint32_t ota_pct_ms;
static uint32_t boot_ms;      /* when the main loop started, for OTA_CHECK_RUNNING */
static bool ota_running_checked;
#endif
/* Sleep (PLAN D17, D82): busy = both PM locks held (nothing scales or sleeps); idle and the keys armed as
 * wake-up sources = released, so the idle task light-sleeps until a key, the 1 s tick, a radio event or a
 * notification. Idle is the screen being off on boards with a backlight, a quiet UI on the Paper. */
static struct {
    bool armed;        /* locks released, keys are wake-up sources */
    uint32_t woke_ms;  /* the last key wake-up */
    uint32_t disarm_ms; /* when the locks were last taken (0 = not yet): the length of an awake stretch */
} slp;
#if CONFIG_EPAGER_FEATURE_WIFI
static net_status_t net_st;     /* last state from the Wi-Fi task */
static uint32_t seen_time, seen_city;
static net_setup_state_t seen_setup;
static bool net_ok, net_config_dirty;
#endif

static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void *alloc_zeroed(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != NULL ? p : heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
}

/* ---------------------------------------------------------------- storage */

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Config, seen apps, Wi-Fi and the bonds all live here and none is precious: a corrupt store is erased
         * whole, the phones pair again (no bonds opens pairing mode, PLAN D6) and the defaults apply. */
        ESP_LOGW(TAG, "NVS unusable (%s), erasing: bonds and settings are lost", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static bool mount_assets(void)
{
    /* A blank or corrupt assets partition is formatted: assets can be uploaded again, and the
     * partition holds nothing else (NVS and the firmware live in their own partitions). */
    esp_vfs_littlefs_conf_t conf = {.base_path = APP_ASSETS_MOUNT, .partition_label = ASSETS_LABEL,
                                    .format_if_mount_failed = true};
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "assets mount failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t total = 0, used = 0;
    if (esp_littlefs_info(ASSETS_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "assets: %u of %u bytes used", (unsigned)used, (unsigned)total);
    }
    return true;
}

static blob_status_t assets_usage(void *ctx, uint32_t *total, uint32_t *free_bytes)
{
    (void)ctx;
    size_t t = 0, used = 0;
    if (esp_littlefs_info(ASSETS_LABEL, &t, &used) != ESP_OK || used > t) {
        return BLOB_ERR;
    }
    *total = (uint32_t)t;
    *free_bytes = (uint32_t)(t - used);
    return BLOB_OK;
}

/* A failed asset write answers the phone with INTERNAL only: the log says where it failed (no app ids). */
static void assets_trace(void *ctx, const char *step, int err)
{
    (void)ctx;
    ESP_LOGW(TAG, "asset write failed at %s: errno %d (%s)", step, err, err != 0 ? strerror(err) : "-");
}

/* ---------------------------------------------------------------- BLE (PLAN D67) and ANCS (D68) */

static void ble_wake(void *ctx)
{
    (void)ctx;
    xTaskNotifyGive(main_task); /* from the NimBLE host task */
}

/* The values a phone reads without the core: Capabilities (before pairing) and Status. Capabilities carry the assets
 * partition's free space, which LittleFS finds by walking the whole file system: about a quarter of a second of CPU
 * at 160 MHz, which once a second kept the chip awake a fifth of the time (PLAN D109). So the loop refreshes Status
 * every tick and Capabilities every CAPS_REFRESH_MS; a change that matters (pairing mode, the first start) refreshes both. */
#define CAPS_REFRESH_MS 60000u
static uint32_t caps_refreshed_ms;

static void refresh_status_value(void)
{
    uint8_t buf[PROTO_LIMIT_CAPABILITIES_MAX];
    blec_set_read_value(CORE_CHR_STATUS, buf, core_read(&core, CORE_CHR_STATUS, buf, sizeof(buf)));
}

static void refresh_read_values(void)
{
    uint8_t buf[PROTO_LIMIT_CAPABILITIES_MAX];
    blec_set_read_value(CORE_CHR_CAPABILITIES, buf, core_read(&core, CORE_CHR_CAPABILITIES, buf, sizeof(buf)));
    caps_refreshed_ms = now_ms(NULL);
    refresh_status_value();
}

static void refresh_read_values_tick(uint32_t t)
{
    if (t - caps_refreshed_ms >= CAPS_REFRESH_MS) {
        refresh_read_values();
    } else {
        refresh_status_value();
    }
}

/* The percentage the status bar shows: whole steps, and gone once the phone has left or stopped sending. */
static int16_t ota_ui_pct(uint32_t t)
{
#if CONFIG_EPAGER_FEATURE_OTA
#if CONFIG_EPAGER_FEATURE_WIFI_OTA
    if (ota_wifi) { /* the Wi-Fi task reports until it ends: no phone link, no staleness */
        return ota_pct < 0 ? -1 : (int16_t)(ota_pct / OTA_PROGRESS_STEP * OTA_PROGRESS_STEP);
    }
#endif
    if (ota_pct >= 0 && (!ble_secure || t - ota_pct_ms > OTA_PROGRESS_STALE_MS)) {
        ota_pct = -1;
    }
    return ota_pct < 0 ? -1 : (int16_t)(ota_pct / OTA_PROGRESS_STEP * OTA_PROGRESS_STEP);
#else
    (void)t;
    return -1;
#endif
}

static void sync_ui_status(void)
{
    ui_status_t s = {.battery_pct = core_battery_pct(&core), .wifi_state = core.status.wifi_state,
                     .external_power = core.status.external_power == PROTO_EXTERNAL_POWER_YES,
                     .connected = ble_secure, .pairing_mode = core.status.pairing_mode,
                     .has_passkey = has_passkey, .passkey = passkey, .ota_pct = ota_ui_pct(now_ms(NULL))};
    ui_set_status(&s);
    if (ble_ok) {
        blec_set_battery(s.battery_pct);
    }
}

/* Pairing mode: new phones may bond (the BLE glue decides), the screen says so, Capabilities tell the app. */
static uint32_t pairing_since_ms;

static void set_pairing(bool on)
{
    pairing_since_ms = now_ms(NULL);
    core_set_pairing_mode(&core, on);
    if (ble_ok) {
        blec_set_pairing_mode(on);
        refresh_read_values();
    }
    sync_ui_status();
}

/* Closes pairing mode after PAIRING_TIMEOUT_MS. A phone entering the passkey or a Wi-Fi setup opened from
 * the pairing screen is in progress: the time starts again once it is over. */
static void pairing_timeout(uint32_t t)
{
    bool busy = has_passkey;
#if CONFIG_EPAGER_FEATURE_WIFI
    busy = busy || seen_setup != NET_SETUP_OFF;
#endif
    if (!core.status.pairing_mode) {
        return;
    }
    if (busy) {
        pairing_since_ms = t;
    } else if (t - pairing_since_ms >= PAIRING_TIMEOUT_MS) {
        ESP_LOGI(TAG, "pairing mode timed out");
        set_pairing(false);
    }
}

static bool ancs_write(void *ctx, const uint8_t *cmd, size_t len)
{
    (void)ctx;
    return blec_ancs_write(cmd, len);
}

/* The user dismissed a card on the board: an iPhone notification gets the negative action (D68). */
static void on_asset(void *ctx, uint8_t asset_type)
{
    (void)ctx;
    ui_assets_changed(asset_type); /* a card on screen picks up an icon installed or removed now */
}

#if CONFIG_EPAGER_FEATURE_CLOCK_FACES
/* CAPABILITIES lists the built-in faces (contract 0.16.0). */
static const char *on_builtin_face(void *ctx, size_t i)
{
    (void)ctx;
    return ui_builtin_face_id(face_colour, i);
}

/* XFER_FINISH of a clock face: the whole file is parsed under the board's rules (ui_face.c). */
static proto_status_t on_check_asset(void *ctx, uint8_t asset_type, const char *id, size_t id_len, const char *blob_name,
                                     uint32_t size)
{
    (void)ctx;
    return asset_type == PROTO_ASSET_TYPE_CLOCK_FACE
               ? ui_face_check(&blob_fs.iface, blob_name, size, id, id_len, face_w, face_h, face_colour)
               : PROTO_OK;
}
#endif

#if CONFIG_EPAGER_FEATURE_OTA
/* What the OTA state machine asks the platform for (xfer_ota.h). Runs on the main task. */
static void on_ota(void *ctx, const xfer_ota_event_t *ev)
{
    (void)ctx;
    switch (ev->kind) {
    case XFER_OTA_EV_PROGRESS:
        ota_pct = ev->pct;
        ota_pct_ms = now_ms(NULL);
        ui_activity(); /* the screen stays on while the image comes in */
        break;
    case XFER_OTA_EV_REBOOT:
        ota_pct = -1;
        npipe_apps_flush(&core.np); /* the seen-apps list is written once a minute: do not lose it to the restart */
        ota_esp_reboot_in(OTA_REBOOT_DELAY_MS);
        break;
    case XFER_OTA_EV_MARK_VALID:
        ota_esp_mark_valid();
        break;
    case XFER_OTA_EV_ROLLBACK:
        ota_esp_rollback();
        break;
    case XFER_OTA_EV_STATE: /* DEVICE_STATUS.ota_state: the core already reports it */
        break;
    }
}

/* A self-check that must hold before the image is kept; no effect unless this image is on probation. */
static void ota_check_passed(uint32_t bits)
{
    if (params.fw != NULL) {
        xfer_ota_check_passed(&core.ota, bits);
    }
}
#endif

static void on_dismissed(void *ctx, const npipe_dismissed_t *d)
{
    (void)ctx;
    if (d->source == PROTO_NOTIF_SOURCE_ANCS && (d->flags & PROTO_NOTIF_FLAGS_NEGATIVE_ACTION)) {
        ancs_sm_negative_action(&ancs, d->ancs_uid);
        ancs_sm_poll(&ancs, now_ms(NULL));
    }
}

/* What the Paper's settings pages list (PLAN D97): the bonded phones, the saved networks (from the Wi-Fi task's
 * status), the About strings. Pushed when a page opens, when the bonds change and when the networks change. */
static ui_info_t ui_info;

static void refresh_info(void)
{
    ui_info.n_phones = 0;
    memset(ui_info.phone_connected, 0, sizeof(ui_info.phone_connected));
    if (ble_ok) {
        ui_info.n_phones = (uint8_t)blec_bonds(ui_info.phone_connected, UI_PHONES_MAX);
    }
    strlcpy(ui_info.firmware, firmware_version, sizeof(ui_info.firmware));
    strlcpy(ui_info.device, blec_device_name(), sizeof(ui_info.device));
    strlcpy(ui_info.board, board->board_id, sizeof(ui_info.board));
    ui_set_info(&ui_info);
}

static void ble_event(const blec_event_t *ev)
{
    const uint32_t t = now_ms(NULL);
    switch (ev->kind) {
    case BLEC_EV_CONNECTED:
        app_ev(APP_EV_BLE_CONNECTED, 0);
        app_batlog_note_ble(1);
        core_connect(&core, ev->mtu ? ev->mtu : CORE_MTU_DEFAULT, false); /* secure comes with BLEC_EV_SECURE */
        break;
    case BLEC_EV_MTU:
        core_set_mtu(&core, ev->mtu);
        break;
    case BLEC_EV_SECURE:
        app_ev(APP_EV_BLE_SECURE, 0);
        app_batlog_note_ble(2);
        core_set_secure(&core, true);
        ble_secure = true;
        refresh_info();
        break;
    case BLEC_EV_DISCONNECTED:
        app_ev(APP_EV_BLE_DISCONNECTED, ev->mtu);
        app_batlog_note_ble(0);
        core_disconnect(&core);
#if CONFIG_EPAGER_FEATURE_OTA
        ota_sink_ble_gone(&ota_sink); /* an update from the phone that was cut off no longer holds the sink */
#endif
        ble_secure = has_passkey = false;
        ancs_sm_session_end(&ancs);
        refresh_info();
        break;
    case BLEC_EV_WRITE:
        core_write(&core, ev->chr, ev->data, ev->len);
        break;
    case BLEC_EV_PASSKEY:
        has_passkey = true;
        passkey = ev->passkey;
        ui_activity(); /* the code must be seen: wake the screen */
        break;
    case BLEC_EV_PAIR_DONE:
        has_passkey = false;
        if (ev->ok) {
            set_pairing(false); /* one phone at a time; the idle screen is back */
        }
        break;
    case BLEC_EV_BONDS:
        core_set_bonded_count(&core, ev->bonds);
        refresh_info();
        if (ev->bonds == 0 && !core.status.pairing_mode) {
            set_pairing(true); /* first boot, or every bond forgotten (PLAN D6) */
        }
        break;
    case BLEC_EV_ANCS_READY:
        ancs_sm_session_start(&ancs);
        break;
    case BLEC_EV_ANCS_LOST:
        ancs_sm_session_end(&ancs);
        break;
    case BLEC_EV_ANCS_NS:
        ancs_sm_on_ns(&ancs, t, ev->data, ev->len);
        break;
    case BLEC_EV_ANCS_DS:
        ancs_sm_on_ds(&ancs, t, ev->data, ev->len);
        break;
    case BLEC_EV_ANCS_WRITE:
        ancs_sm_write_done(&ancs, t, ev->ok);
        break;
    default:
        break;
    }
}

/* Main loop: take what the radio queued, run the ANCS timers, send what the core queued. True while the
 * radio has more to do (the loop does not sleep). */
static bool ble_service(void)
{
    if (!ble_ok) {
        return false;
    }
    bool any = false;
    while (blec_poll(&ble_ev)) {
        ble_event(&ble_ev);
        any = true;
    }
    ancs_sm_poll(&ancs, now_ms(NULL));
    blec_set_busy(ancs_sm_busy(&ancs));
    if (any) {
        sync_ui_status();
        refresh_read_values();
    }
    return blec_pump();
}

/* ---------------------------------------------------------------- core and UI */

static void sync_ui_config(void)
{
    const cfg_store_t *c = &core.cfg;
    ui_config_t u = {.language = (uint8_t)cfg_get_int(c, PROTO_CFG_UI_LANGUAGE),
                     .show_body = cfg_get_int(c, PROTO_CFG_DISPLAY_SHOW_BODY) != 0,
                     .mask_numbers = cfg_get_int(c, PROTO_CFG_DISPLAY_MASK_NUMBERS) != 0,
                     .full_refresh_every = (uint8_t)cfg_get_int(c, PROTO_CFG_DISPLAY_FULL_REFRESH_EVERY),
                     .tz_offset_min = (int16_t)cfg_get_int(c, PROTO_CFG_TIME_TZ_OFFSET_MIN),
                     .clock_24h = cfg_get_int(c, PROTO_CFG_UI_CLOCK_24H) != 0,
                     .temp_unit = (uint8_t)cfg_get_int(c, PROTO_CFG_UI_TEMP_UNIT),
                     .brightness = (uint8_t)cfg_get_int(c, PROTO_CFG_DISPLAY_BRIGHTNESS),
                     .idle_dim_s = (uint16_t)cfg_get_int(c, PROTO_CFG_DISPLAY_IDLE_DIM_S),
                     .idle_off_s = (uint16_t)cfg_get_int(c, PROTO_CFG_DISPLAY_IDLE_OFF_S),
                     .orientation = (uint8_t)cfg_get_int(c, PROTO_CFG_DISPLAY_ORIENTATION),
                     .weather_enabled = cfg_get_int(c, PROTO_CFG_WEATHER_ENABLED) != 0,
                     .update_auto_check = cfg_get_int(c, PROTO_CFG_UPDATE_AUTO_CHECK) != 0};
    const uint8_t *str;
    size_t n = cfg_get_str(c, PROTO_CFG_UI_CLOCK_FACE_ID, &str);
    memcpy(u.clock_face_id, str, n < sizeof(u.clock_face_id) - 1 ? n : sizeof(u.clock_face_id) - 1);
    n = cfg_get_str(c, PROTO_CFG_UI_CLOCK_FACE_OPTS, &str);
    u.clock_face_opts_len = (uint8_t)(n < sizeof(u.clock_face_opts) ? n : sizeof(u.clock_face_opts));
    memcpy(u.clock_face_opts, str, u.clock_face_opts_len);
    ui_set_config(&u);
}

/* ---------------------------------------------------------------- Wi-Fi and weather (D60-D63) */

#if CONFIG_EPAGER_FEATURE_WIFI
static uint32_t now_ms_void(void) { return now_ms(NULL); }

static proto_status_t on_wifi_set(void *ctx, const uint8_t *ssid, size_t ssid_len, const uint8_t *pw, size_t pw_len,
                                  bool forget)
{
    (void)ctx;
    return net_wifi_set(ssid, ssid_len, pw, pw_len, forget);
}

/* Hand the weather settings to the Wi-Fi task; retried from the main loop while its queue is full. */
static void sync_net_config(void)
{
    const uint8_t *city;
    const size_t n = cfg_get_str(&core.cfg, PROTO_CFG_WEATHER_CITY, &city);
    net_config_dirty = net_ok && !net_configure(WEATHER_ON && cfg_get_int(&core.cfg, PROTO_CFG_WEATHER_ENABLED) != 0,
                                                (uint16_t)cfg_get_int(&core.cfg, PROTO_CFG_WEATHER_INTERVAL_MIN),
                                                city, n, (uint8_t)cfg_get_int(&core.cfg, PROTO_CFG_UI_LANGUAGE));
}

#if CONFIG_EPAGER_FEATURE_WIFI_OTA
/* ---------------------------------------------------------------- firmware updates over Wi-Fi (M6c) */

static void update_start(const char *user_agent)
{
    if (!net_ok || params.fw == NULL || upd_kv == NULL) {
        return;
    }
    const net_update_cfg_t nc = {.board_id = board->board_id, .variant = app->variant, .running_version = firmware_version,
                                 .contract_version = PROTO_CONTRACT_VERSION, .sink = &ota_sink.wifi,
                                 .slot_size = ota_sink.flash->slot_size, .user_agent = user_agent};
    net_update_start(&nc);
    uint32_t jitter = 0; /* boards do not call the release host at the same second */
    for (const char *p = blec_device_name(); *p != '\0'; p++) {
        jitter = jitter * 31u + (uint8_t)*p;
    }
    const upd_policy_params_t pp = {.kv = upd_kv, .running_version = firmware_version, .jitter_s = jitter % 1800u,
                                    .period_s = 24u * 3600u, .retry_s = 3600u, .max_tries = 6, .first_delay_s = 3600u,
                                    .snooze_s = 24u * 3600u};
    upd_policy_init(&upd, &pp, cfg_get_int(&core.cfg, PROTO_CFG_UPDATE_AUTO_CHECK) != 0);
    upd_ready = true;
}

/* The person asked (the settings page, the console): check now. */
static bool update_check_now(void)
{
    upd_local_err = UI_UPD_ERR_NONE;
    if (!upd_ready || upd_check_inflight || !net_update_check(true)) {
        return false;
    }
    upd_check_inflight = true;
    return true;
}

/* Install the newest version found. The battery and version rules are the BLE update's (xfer_ota). */
static bool update_install(void)
{
    const char *v = upd_ready ? upd_policy_available(&upd) : "";
    upd_local_err = UI_UPD_ERR_NONE;
    if (v[0] == '\0') {
        return false;
    }
    const proto_status_t gate = xfer_ota_check_begin(&core.ota, v, strlen(v), true);
    if (gate != PROTO_OK) {
        ESP_LOGW(TAG, "update refused: %s", gate == PROTO_LOW_BATTERY ? "battery" : "version");
        upd_local_err = gate == PROTO_LOW_BATTERY ? UI_UPD_ERR_BATTERY : UI_UPD_ERR_INTERNAL;
        return false;
    }
    if (!net_update_install(v)) {
        upd_local_err = UI_UPD_ERR_INTERNAL; /* the Wi-Fi task could not take it now */
        return false;
    }
    return true;
}

static ui_update_error_t ui_error_for(uint8_t net_error)
{
    switch (net_error) {
    case NET_UPD_ERR_NO_NETWORK: return UI_UPD_ERR_NO_NETWORK;
    case NET_UPD_ERR_NET: return UI_UPD_ERR_NET;
    case NET_UPD_ERR_CLOCK: return UI_UPD_ERR_CLOCK;
    case NET_UPD_ERR_DOWNLOAD: return UI_UPD_ERR_DOWNLOAD;
    case NET_UPD_ERR_REJECTED: return UI_UPD_ERR_REJECTED;
    case NET_UPD_ERR_BUSY: return UI_UPD_ERR_BUSY;
    case NET_UPD_ERR_CANCELLED: return UI_UPD_ERR_CANCELLED;
    default: return UI_UPD_ERR_INTERNAL;
    }
}

/* What the Software update page and the clock screen's mark show: the policy's record and the Wi-Fi task's state. */
static void update_sync_ui(void)
{
    if (!upd_ready) {
        return;
    }
    ui_update_t u;
    memset(&u, 0, sizeof(u));
    const char *avail = upd_policy_available(&upd);
    const bool skipped = upd_policy_skipped(&upd);
    strlcpy(u.version, avail, sizeof(u.version));
    u.skipped = skipped;
    u.mark = avail[0] != '\0' && !skipped;
    if (upd_local_err != UI_UPD_ERR_NONE) {
        u.state = UI_UPD_FAILED;
        u.error = upd_local_err;
    } else if (net_st.upd_state == NET_UPD_DOWNLOADING || net_st.upd_state == NET_UPD_INSTALLED) {
        u.state = UI_UPD_DOWNLOADING;
    } else if (net_st.upd_state == NET_UPD_CHECKING || upd_check_inflight) {
        u.state = UI_UPD_CHECKING;
    } else if (net_st.upd_state == NET_UPD_FAILED) {
        u.state = UI_UPD_FAILED;
        u.error = ui_error_for(net_st.upd_error);
    } else if (avail[0] != '\0') {
        u.state = UI_UPD_AVAILABLE;
        if (strcmp(net_st.upd_version, avail) == 0) {
            u.size = net_st.upd_size;
        }
    } else if (upd.last_verdict == UPD_CONTRACT_MAJOR) {
        u.state = UI_UPD_NEEDS_APP;
        strlcpy(u.version, net_st.upd_version, sizeof(u.version));
    } else {
        u.state = upd.r.last_check_utc != 0 ? UI_UPD_UP_TO_DATE : UI_UPD_NONE;
    }
    ui_set_update(&u);
}

/* Once a second: a check is due, and the prompt may be due (it waits for an awake, idle clock screen). */
static void update_tick(uint32_t t)
{
    if (!upd_ready) {
        return;
    }
    const uint32_t utc = core_now_utc(&core);
    if (!upd_check_inflight && upd_policy_check_due(&upd, utc, t / 1000u) && net_update_check(false)) {
        upd_check_inflight = true;
    }
    update_sync_ui();
    if (upd_policy_prompt_due(&upd, utc) && ui_show_update_prompt()) {
        upd_policy_prompt_shown(&upd, utc); /* unanswered counts as "remind later" */
        ESP_LOGI(TAG, "update: prompt shown for %s", upd_policy_available(&upd));
    }
}

/* The Software update page and its prompt. */
static void update_action(ui_action_t a)
{
    if (!upd_ready) {
        return;
    }
    switch (a) {
    case UI_ACTION_UPDATE_CHECK: (void)update_check_now(); break;
    case UI_ACTION_UPDATE_INSTALL: (void)update_install(); break;
    case UI_ACTION_UPDATE_LATER: upd_policy_remind_later(&upd, core_now_utc(&core)); break;
    case UI_ACTION_UPDATE_SKIP: upd_policy_skip(&upd); break;
    case UI_ACTION_UPDATE_CANCEL: net_update_cancel(); break;
    default: break;
    }
    update_sync_ui();
}

/* The Wi-Fi task's news about an update. */
static void update_apply_net(const net_status_t *s)
{
    if (!upd_ready) {
        return;
    }
    const uint32_t utc = core_now_utc(&core);
    if (s->upd_check_seq != upd_seq_seen) {
        upd_seq_seen = s->upd_check_seq;
        if (s->upd_check_ok) {
            upd_policy_checked(&upd, utc, (upd_verdict_t)s->upd_verdict, s->upd_version);
        } else if (upd_check_inflight) {
            upd_policy_check_failed(&upd, utc);
        }
        upd_check_inflight = false;
    }
    if (s->upd_state != upd_net_state_seen) {
        upd_net_state_seen = s->upd_state;
        upd_local_err = UI_UPD_ERR_NONE; /* the Wi-Fi task has news: it supersedes an earlier local refusal */
    }
    ota_wifi = s->upd_state == NET_UPD_DOWNLOADING;
    if (ota_wifi) {
        xfer_ota_progress(&core.ota, s->upd_pct, 100); /* the progress event shows it and keeps the screen on */
    } else if (ota_pct >= 0 && s->upd_state != NET_UPD_INSTALLED) {
        ota_pct = -1;
    }
    if (s->upd_state == NET_UPD_INSTALLED && !upd_installed_seen) {
        upd_installed_seen = true;
        upd_policy_installed(&upd);
        xfer_ota_installed(&core.ota, s->upd_version, strlen(s->upd_version)); /* the record, then the restart */
    }
    update_sync_ui();
}

/* The debug console's `update` command (main loop). */
#if CONFIG_EPAGER_FEATURE_CLOCK_FACES
static void print_face(void *ctx, const char *id, const char *name, bool builtin, bool local, bool active)
{
    (void)ctx;
    ESP_LOGI(TAG, "face %c %-24s %s%s%s", active ? '*' : ' ', id[0] ? id : "(classic)", name, builtin ? " [built in]" : "",
             local ? " [from a file]" : "");
}

/* The console's `face` command: list, select ID, opt N V, delete ID (the settings pages' actions without the screen). */
static void face_command(const char *word, const char *id, uint8_t option, uint8_t value)
{
    if (strcmp(word, "select") == 0) {
        ESP_LOGI(TAG, "console: face select '%s' -> status %d", id, core_face_select(&core, id));
    } else if (strcmp(word, "opt") == 0) {
        ESP_LOGI(TAG, "console: face option %u = %u -> status %d", option, value, core_face_option(&core, option, value));
    } else if (strcmp(word, "delete") == 0) {
        ESP_LOGI(TAG, "console: face delete '%s' -> status %d", id, core_face_delete(&core, id));
    } else {
        ui_face_list(print_face, NULL);
    }
}
#endif

static void update_command(const char *word, const char *arg, uint16_t number)
{
    if (!upd_ready) {
        ESP_LOGI(TAG, "update: not available (no Wi-Fi, no second OTA slot, or the task did not start)");
        return;
    }
    if (strcmp(word, "check") == 0) {
        ESP_LOGI(TAG, "update: check %s", update_check_now() ? "queued" : "not queued");
    } else if (strcmp(word, "install") == 0) {
        ESP_LOGI(TAG, "update: install %s", update_install() ? "queued" : "refused");
    } else if (strcmp(word, "cancel") == 0) {
        net_update_cancel();
    } else if (strcmp(word, "later") == 0) {
        upd_policy_remind_later(&upd, core_now_utc(&core));
    } else if (strcmp(word, "skip") == 0) {
        upd_policy_skip(&upd);
    } else if (strcmp(word, "due") == 0) { /* the daily check now, as if a day had passed */
        ESP_LOGI(TAG, "update: daily check %s", !upd_check_inflight && net_update_check(false) ? "queued" : "not queued");
        upd_check_inflight = true;
    } else if (strcmp(word, "url") == 0) {
        net_update_set_url(arg);
    } else if (strcmp(word, "auto") == 0) { /* update.auto_check, as the app or the settings row sets it */
        core_config_set_u8(&core, PROTO_CFG_UPDATE_AUTO_CHECK, number != 0);
    } else if (strcmp(word, "drop") == 0) { /* the next download loses its connection once after N KiB */
        net_update_debug_drop((uint32_t)number * 1024u);
    } else if (strcmp(word, "interval") == 0 && number > 0) { /* a short "day" for tests */
        upd.p.period_s = number * 60u;
        upd.p.jitter_s = 0;
        upd.p.first_delay_s = 0;
    }
    ESP_LOGI(TAG, "update: auto %d, available '%s'%s, last check %lu, retries %u, period %lu s, remind until %lu, now %lu",
             (int)upd.auto_check, upd_policy_available(&upd), upd_policy_skipped(&upd) ? " (skipped)" : "",
             (unsigned long)upd.r.last_check_utc, (unsigned)upd.r.retries, (unsigned long)upd.p.period_s,
             (unsigned long)upd.r.remind_until_utc, (unsigned long)core_now_utc(&core));
}
#endif

/* What the Wi-Fi task reported: DEVICE_STATUS, the weather row, the setup screen, the time (SNTP or a
 * Date header; unless a
 * phone set it within a day), the weather location's time zone (same rule) and a city from the setup page.
 * A successful Wi-Fi setup also ends pairing mode: the hotspot was opened from the pairing screen, and
 * the "Connected" screen leads back to the idle screen instead of the pairing one. */
static void apply_net(void)
{
    net_status_t *s = &net_st;
    if (net_config_dirty) {
        sync_net_config();
    }
    if (!net_poll(s)) {
        return;
    }
#if CONFIG_EPAGER_FEATURE_WIFI_OTA
    update_apply_net(s);
#endif
    ui_info.n_networks = s->n_networks > UI_NETWORKS_MAX ? UI_NETWORKS_MAX : s->n_networks;
    memcpy(ui_info.networks, s->networks, sizeof(ui_info.networks));
    refresh_info();
    app_batlog_note_wifi(s->wifi_state);
    core_set_wifi_status(&core, s->wifi_state, s->ssid);
    core_set_weather_state(&core, s->weather_state);
    ui_set_weather(WEATHER_ON && s->have_reading ? &s->reading : NULL);
    ui_wifi_setup_t u = {.state = (ui_setup_state_t)s->setup};
    if (s->setup != NET_SETUP_OFF) {
        strlcpy(u.ap_ssid, s->ap_ssid, sizeof(u.ap_ssid));
        strlcpy(u.ap_password, s->ap_password, sizeof(u.ap_password));
        strlcpy(u.url, s->setup_url, sizeof(u.url));
        strlcpy(u.network, s->setup_network, sizeof(u.network));
    }
    ui_set_wifi_setup(&u);
    if (s->setup == NET_SETUP_DONE && seen_setup != NET_SETUP_DONE && core.status.pairing_mode) {
        set_pairing(false);
    }
    seen_setup = s->setup;
    const bool phone_time = core_time_sync_age_ms(&core) < PHONE_TIME_MS;
    if (s->time_seq != seen_time) {
        seen_time = s->time_seq;
        if (!phone_time) {
            core_set_utc(&core, s->time_utc + (now_ms(NULL) - s->time_ms) / 1000u);
            ui_set_time(core_now_utc(&core));
        }
    }
    if (s->have_reading && s->reading.has_utc_offset && !phone_time) {
        const int16_t tz = (int16_t)(s->reading.utc_offset_s / 60);
        if (tz != cfg_get_int(&core.cfg, PROTO_CFG_TIME_TZ_OFFSET_MIN)) {
            const uint8_t v[2] = {(uint8_t)tz, (uint8_t)((uint16_t)tz >> 8)}; /* little-endian, the wire */
            core_config_set(&core, PROTO_CFG_TIME_TZ_OFFSET_MIN, v, sizeof(v));
        }
    }
    if (s->city_seq != seen_city) {
        seen_city = s->city_seq;
        core_config_set(&core, PROTO_CFG_WEATHER_CITY, (const uint8_t *)s->city, strlen(s->city));
    }
}
#endif

static void on_config(void *ctx, const uint16_t *changed, size_t n_keys)
{
    (void)ctx;
    sync_ui_config();
#if CONFIG_EPAGER_FEATURE_WIFI_OTA
    for (size_t i = 0; i < n_keys; i++) {
        if (changed[i] == PROTO_CFG_UPDATE_AUTO_CHECK && upd_ready) {
            upd_policy_set_auto_check(&upd, cfg_get_int(&core.cfg, PROTO_CFG_UPDATE_AUTO_CHECK) != 0);
        }
    }
#endif
#if CONFIG_EPAGER_FEATURE_WIFI
    for (size_t i = 0; i < n_keys; i++) {
        if (changed[i] == PROTO_CFG_UI_LANGUAGE || (changed[i] >= PROTO_CFG_WEATHER_ENABLED &&
                                                    changed[i] <= PROTO_CFG_WEATHER_INTERVAL_MIN)) {
            sync_net_config();
            break;
        }
    }
#else
    (void)changed;
    (void)n_keys;
#endif
}

/* The phone changed a setting: say so on the screen for a moment (PLAN D79). */
static void on_setting(void *ctx, core_setting_t kind)
{
    (void)ctx;
    ui_show_notice(kind == CORE_SETTING_WIFI ? UI_NOTICE_WIFI : UI_NOTICE_SETTING);
}

static void on_notif(void *ctx, const npipe_notif_t *n)
{
    (void)ctx;
    ui_on_notification(n);
}

/* Power off (PLAN D88): show the "Powered off" screen, drop the phone, and once the panel has finished the
 * board switches itself off (main loop, power_off_poll). The key that asked for it is still down at that point. */
static struct {
    bool pending;
    uint32_t since_ms;
} off;
#define POWER_OFF_MIN_MS 500    /* the panel's refresh is not over before the UI reports idle */
#define POWER_OFF_MAX_MS 15000  /* a refresh that never ends must not keep the board on */

static void start_power_off(void)
{
    if (off.pending || board->power_off == NULL) {
        return;
    }
    ESP_LOGI(TAG, "power off requested");
    off.pending = true;
    off.since_ms = now_ms(NULL);
    if (ble_ok) {
        blec_disconnect();
    }
    ui_set_power(false);
}

static void power_off_poll(uint32_t t, bool ui_pending)
{
    if (off.pending && t - off.since_ms >= POWER_OFF_MIN_MS && (!ui_pending || t - off.since_ms >= POWER_OFF_MAX_MS)) {
        board->power_off(board->ctx); /* returns only if the board could not switch off */
        off.pending = false;
        ui_set_power(true);
    }
}

#if CONFIG_EPAGER_BOOT_SOUND
static int16_t chime_sample(void *user, uint32_t i)
{
    (void)user;
    return board_chime_sample(i, BOARD_CHIME_RATE_HZ);
}
#if CONFIG_EPAGER_SOUND_TEST_VOICE
extern const uint8_t voice_pcm_start[] asm("_binary_voice_test_pcm_start");
extern const uint8_t voice_pcm_end[] asm("_binary_voice_test_pcm_end");
static int16_t voice_sample(void *user, uint32_t i)
{
    (void)user;
    return (int16_t)(voice_pcm_start[2 * i] | voice_pcm_start[2 * i + 1] << 8); /* 16 bit little endian */
}
#endif
#if CONFIG_EPAGER_SOUND_TEST
static int16_t test_sample(void *user, uint32_t i)
{
    (void)user;
    return board_test_sample(i, BOARD_CHIME_RATE_HZ);
}
#endif
#endif

/* What the firmware does with a UI action (keys and touch alike); same task as the core. */
static void on_ui_action(void *ctx, ui_action_t a, uint32_t arg)
{
    (void)ctx;
    if (off.pending) {
        return;
    }
    switch (a) {
    case UI_ACTION_PAIRING_TOGGLE:
        set_pairing(!core.status.pairing_mode);
        break;
    case UI_ACTION_DISMISS:
        core_dismiss(&core, arg);
        break;
    case UI_ACTION_BRIGHTNESS_UP:
    case UI_ACTION_BRIGHTNESS_DOWN: {
        const uint8_t now = (uint8_t)cfg_get_int(&core.cfg, PROTO_CFG_DISPLAY_BRIGHTNESS);
        core_config_set_u8(&core, PROTO_CFG_DISPLAY_BRIGHTNESS, ui_brightness_step(now, a == UI_ACTION_BRIGHTNESS_UP));
        break;
    }
#if CONFIG_EPAGER_FEATURE_WIFI
    case UI_ACTION_WIFI_SETUP:
    case UI_ACTION_WIFI_SETUP_CLOSE:
        net_setup(a == UI_ACTION_WIFI_SETUP);
        break;
#endif
    case UI_ACTION_INFO_REFRESH:
        refresh_info();
        break;
    case UI_ACTION_FORGET_PHONE:
        if (ble_ok) {
            blec_forget_bond(arg); /* BLEC_EV_BONDS follows; the last one opens pairing mode */
        }
        break;
#if CONFIG_EPAGER_FEATURE_WIFI
    case UI_ACTION_FORGET_NETWORK:
        if (arg < ui_info.n_networks) { /* the Wi-Fi task reports the shorter list when it is done */
            (void)net_wifi_set((const uint8_t *)ui_info.networks[arg], strlen(ui_info.networks[arg]), NULL, 0, true);
        }
        break;
#endif
    case UI_ACTION_SETTING: /* the Paper's settings list: a config key and its new value (D96) */
        core_config_set_u8(&core, (uint16_t)(arg >> 8), (uint8_t)arg);
        break;
    case UI_ACTION_POWER_TOGGLE:
        start_power_off();
        break;
#if CONFIG_EPAGER_FEATURE_CLOCK_FACES
    case UI_ACTION_FACE_SELECT:
        (void)core_face_select(&core, ui_face_list_id(arg));
        break;
    case UI_ACTION_FACE_OPTION:
        (void)core_face_option(&core, (uint8_t)(arg >> 8), (uint8_t)arg);
        break;
    case UI_ACTION_FACE_DELETE:
        (void)core_face_delete(&core, ui_face_list_id(arg));
        break;
#endif
#if CONFIG_EPAGER_FEATURE_WIFI_OTA
    case UI_ACTION_UPDATE_CHECK:
    case UI_ACTION_UPDATE_INSTALL:
    case UI_ACTION_UPDATE_LATER:
    case UI_ACTION_UPDATE_SKIP:
    case UI_ACTION_UPDATE_CANCEL:
        update_action(a);
        break;
#endif
    default:
        break;
    }
}

/* Interrupt context (the key wake-up from light sleep): wake the main loop. */
static void on_wake_isr(void *user)
{
    (void)user;
    BaseType_t woken = pdFALSE;
    key_ev_t ev = {EV_WAKE, BOARD_PRESS_SHORT};
    xQueueSendFromISR(keys, &ev, &woken);
    vTaskNotifyGiveFromISR(main_task, &woken);
    portYIELD_FROM_ISR(woken);
}

/* Hold the locks while the app is busy or a key just woke the chip; release them once it is idle and the keys
 * can wake it. Idle = the screen is off (boards with a backlight), or nothing is pending and the radio is quiet
 * (the Paper: its panel keeps the picture without power, so the chip sleeps right after a refresh, PLAN D82, and
 * through the refresh itself when the panel can wake it, D123). */
static void sleep_policy(uint32_t t, bool quiet)
{
    const bool backlight = board->display != NULL && board->display->set_backlight != NULL;
    if (!backlight && board->set_sleep == NULL) {
        return;
    }
    const bool want = (backlight ? ui_screen_off() : quiet) && t - slp.woke_ms >= REARM_MS;
    if (want && !slp.armed) {
        if (board->set_sleep != NULL && !board->set_sleep(board->ctx, true, on_wake_isr, NULL)) {
            return; /* a key is down: the polling times the press, ask again on the next pass */
        }
        slp.armed = true;
        if (slp.disarm_ms != 0) {
            app_batlog_note_awake(t - slp.disarm_ms);
        }
        app_ev(APP_EV_SLEEP_ARM, 0);
        power_release(POWER_LOCK_NO_SLEEP);
        power_release(POWER_LOCK_CPU_MAX);
        if (backlight) {
            ESP_LOGI(TAG, "screen off: light sleep allowed");
        } else {
            ESP_LOGD(TAG, "idle: light sleep allowed"); /* every minute on the Paper: not worth a line */
        }
    } else if (!want && slp.armed) {
        power_hold(POWER_LOCK_CPU_MAX);
        power_hold(POWER_LOCK_NO_SLEEP);
        if (board->set_sleep != NULL) {
            board->set_sleep(board->ctx, false, NULL, NULL);
        }
        slp.armed = false;
        slp.disarm_ms = t != 0 ? t : 1;
        app_ev(APP_EV_SLEEP_DISARM, 0);
        app_batlog_note_wake();
    }
}

/* Interrupt context: the panel finished its refresh (PLAN D123). Wake the main loop to push the next frame or put
 * the panel to sleep; not a key, so the locks stay released. */
static void on_panel_idle_isr(void *user)
{
    (void)user;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(main_task, &woken);
    portYIELD_FROM_ISR(woken);
}

/* The Pebble kept its chip awake for the SPI transfer of a frame, not for the panel's update (PLAN D123): while
 * the chip may sleep and only the panel is working, the end of the refresh wakes it. Returns whether that wake-up
 * is armed; when it is not (the panel is not busy yet), the loop polls at the busy rate, still allowed to sleep. */
static bool panel_wake_policy(bool panel_wait)
{
    static bool armed;
    const board_display_t *d = board->display;
    if (d == NULL || d->idle_wake == NULL) {
        return false;
    }
    if (panel_wait && slp.armed) {
        armed = d->idle_wake(d->ctx, true, on_panel_idle_isr, NULL);
    } else if (armed) {
        d->idle_wake(d->ctx, false, NULL, NULL);
        armed = false;
    }
    return armed;
}

/* esp_timer task: only queue the event for the main loop. */
static void on_button(void *user, board_button_t btn, board_press_t press)
{
    (void)user;
    key_ev_t ev = {btn, press};
    xQueueSend(keys, &ev, 0);
    xTaskNotifyGive(main_task);
}

static void poll_climate(void)
{
    board_climate_t c;
    if (board->read_climate != NULL) {
        ui_set_climate(board->read_climate(board->ctx, &c) ? &c : NULL);
    }
}

/* core's `notify`: one value for a characteristic, to the BLE glue (queued there). */
static void on_core_notify(void *ctx, uint8_t chr, const uint8_t *val, size_t len)
{
    (void)ctx;
    blec_notify(chr, val, len);
}

static bool init_core_and_ui(bool kv_nvs_ok, bool assets_ok)
{
    board_display_info_t di;
    board->display->get_info(board->display->ctx, &di);

    npipe_slot_t *slots = alloc_zeroed(PROTO_LIMIT_NOTIF_HISTORY_MAX, sizeof(*slots));
    npipe_app_t *apps = alloc_zeroed(PROTO_LIMIT_SEEN_APPS_MAX, sizeof(*apps));
    if (slots == NULL || apps == NULL) {
        return false;
    }
    kv_storage_t *kv = &kv_nvs.iface;
    if (!kv_nvs_ok) {
        ESP_LOGW(TAG, "settings are kept in RAM only");
        kv_mem_init(&kv_ram);
        kv = &kv_ram.iface;
    }
    params = (core_params_t){
        .device_name = blec_device_name(),
        .board_id = board->board_id,
        .firmware_version = firmware_version,
        .variant = app->variant,
        .display_type = di.display_type,
        .color_depth = di.color_depth,
        .refresh_class = di.refresh_class,
        .display_w = di.width,
        .display_h = di.height,
        .features = board->features | PROTO_FEATURES_ANCS | PROTO_FEATURES_ANDROID_NOTIFICATIONS,
        .transports = PROTO_TRANSPORTS_BLE,
        .max_mtu = CORE_MTU_MAX,
        .kv_cfg = kv, .kv_apps = kv, .kv_xfer = kv,
        .low_battery_pct = board->low_battery_ota_pct,
        .ack_window = 4096, .journal_every = 16384,
        .slots = slots, .n_slots = PROTO_LIMIT_NOTIF_HISTORY_MAX,
        .apps = apps, .n_apps = PROTO_LIMIT_SEEN_APPS_MAX,
        .now_ms = now_ms,
        .read_battery = board->read_battery,
        .battery_poll_ms = 60000,
        .apps_flush_ms = 60000,
        .rtc_get = board->rtc_get, /* the clock survives resets when the board has an RTC */
        .rtc_set = board->rtc_set,
        .on_config = on_config,
        .on_setting = on_setting,
        .notify = on_core_notify,
        .on_notif = on_notif,
        .on_dismissed = on_dismissed,
        .ctx = board->ctx, /* the board's battery reader and RTC get the board's context */
    };
#if CONFIG_EPAGER_FEATURE_WIFI
    params.features |= PROTO_FEATURES_WIFI | (WEATHER_ON ? PROTO_FEATURES_WEATHER : 0u);
    params.wifi_set = on_wifi_set;
#endif
    if (assets_ok && blob_fs_init(&blob_fs, APP_ASSETS_MOUNT, assets_usage, NULL)) {
        blob_fs_set_trace(&blob_fs, assets_trace, NULL);
        /* The size and format come from the display: asset-formats.md "App icons" (Paper 24x24 mono1, LCD 32x32 rgb565a8). */
        asset_support[0] = di.color_depth > 1
                               ? (xfer_asset_support_t){PROTO_ASSET_TYPE_APP_ICON, PROTO_ASSET_FORMAT_RGB565A8, 3088, 32, 32, 0, 0}
                               : (xfer_asset_support_t){PROTO_ASSET_TYPE_APP_ICON, PROTO_ASSET_FORMAT_MONO1, 88, 24, 24, 0, 0};
        params.blob = &blob_fs.iface;
        params.assets = asset_support;
        params.n_assets = 1;
        params.features |= PROTO_FEATURES_ASSETS;
        params.on_asset = on_asset;
#if CONFIG_EPAGER_FEATURE_CLOCK_FACES
        /* asset-formats.md "Clock faces": file size Paper 64 KiB / LCD 1 MiB, budget 1 MiB / 4 MiB, 16 faces */
        face_w = di.width;
        face_h = di.height;
        face_colour = di.color_depth > 1;
        asset_support[params.n_assets++] =
            face_colour ? (xfer_asset_support_t){PROTO_ASSET_TYPE_CLOCK_FACE, PROTO_ASSET_FORMAT_CLOCK_FACE_V1, 1048576u, 0, 0,
                                                 4194304u, 16}
                        : (xfer_asset_support_t){PROTO_ASSET_TYPE_CLOCK_FACE, PROTO_ASSET_FORMAT_CLOCK_FACE_V1, 65536u, 0, 0,
                                                 1048576u, 16};
        params.check_asset = on_check_asset;
        params.builtin_face = on_builtin_face;
#endif
    } else {
        ESP_LOGW(TAG, "no asset storage: cards show the built-in icons");
    }
#if CONFIG_EPAGER_FEATURE_OTA
    const ota_flash_t *ota_flash = ota_esp_flash();
    if (ota_flash != NULL) {
        ota_sink_init(&ota_sink, ota_flash);
        params.fw = &ota_sink.iface;
        params.features |= PROTO_FEATURES_OTA;
        params.ota_required_checks = OTA_REQUIRED;
        params.ota_verify_timeout_ms = OTA_VERIFY_TIMEOUT_MS;
        params.on_ota = on_ota;
    }
#endif
    core_init(&core, &params);
#if CONFIG_EPAGER_FEATURE_WIFI_OTA
    upd_kv = kv;
#endif
#if CONFIG_EPAGER_FEATURE_OTA
    const bool probation = ota_esp_pending_verify();
    if (probation) {
        ESP_LOGW(TAG, "new firmware on probation: self-check within %u s", (unsigned)(OTA_VERIFY_TIMEOUT_MS / 1000u));
    }
    core_boot(&core, probation);
#else
    core_boot(&core, false);
#endif
    ancs_sm_init(&ancs, &(ancs_sm_params_t){.np = &core.np, .write_cp = ancs_write});

    ui_params_t up = {.display = board->display, .blob = params.blob, .climate = board->read_climate != NULL, .now_ms = now_ms,
                      .read_touch = board->read_touch, .touch_ctx = board->ctx, .on_action = on_ui_action};
    up.cards_timeout_s = UI_CARDS_TIMEOUT_S; /* a minute without a key, a touch or a notification: the clock (D80) */
#if CONFIG_EPAGER_FEATURE_WIFI
    up.wifi_setup = true; /* the pairing screen offers it: a button on touch screens, short BOOT on the Paper */
#endif
#if !CONFIG_EPAGER_FEATURE_CLOCK_FACES
    up.no_faces = true;
#endif
#if CONFIG_EPAGER_FEATURE_WIFI_OTA
    up.update = params.fw != NULL; /* the Software update rows need a second OTA slot */
#endif
    if (!ui_init(&up)) {
        return false;
    }
    sync_ui_config();
    sync_ui_status();
    refresh_info();
    ui_set_time(core_now_utc(&core));
    poll_climate();
    keys = xQueueCreate(8, sizeof(key_ev_t));
    if (board->set_button_handler != NULL && keys != NULL) {
        board->set_button_handler(board->ctx, on_button, NULL);
    }
    return true;
}

static void feature_hooks(void)
{
#if CONFIG_EPAGER_FEATURE_ANIMATION
    ui_anim_init();
#endif
#if CONFIG_EPAGER_FEATURE_TOUCH
    ui_touch_init();
#endif
}

/* ---------------------------------------------------------------- debug console commands (D57) */

void app_console_apply(const app_console_cmd_t *cmd)
{
    static uint32_t seq;
    char key[16];
    switch (cmd->kind) {
    case APP_CMD_AB:
        app_ab_apply(board, cmd);
        break;
    case APP_CMD_BATLOG:
        if (cmd->on) {
            app_batlog_clear();
        } else {
            app_batlog_dump();
        }
        break;
    case APP_CMD_POST: {
        snprintf(key, sizeof(key), "dbg-%u", (unsigned)++seq);
        const char *app_id = "debug.console", *name = "Debug";
        npipe_android_t a = {.key = {(const uint8_t *)key, strlen(key)},
                             .app_id = {(const uint8_t *)app_id, strlen(app_id)},
                             .app_name = {(const uint8_t *)name, strlen(name)},
                             .title = {(const uint8_t *)cmd->title, strlen(cmd->title)},
                             .body = {(const uint8_t *)cmd->body, strlen(cmd->body)},
                             .category = cmd->category, .priority = PROTO_PRIORITY_NORMAL};
        uint32_t id;
        bool shown;
        proto_status_t st = npipe_android_post(&core.np, &a, &id, &shown);
        ESP_LOGI(TAG, "console: post %s -> status %d, id %u%s", key, st, (unsigned)id, shown ? "" : " (hidden)");
        break;
    }
    case APP_CMD_REMOVE:
        snprintf(key, sizeof(key), "dbg-%u", (unsigned)seq);
        if (seq && npipe_android_remove(&core.np, (const uint8_t *)key, strlen(key)) == PROTO_OK) {
            seq--;
        }
        break;
    case APP_CMD_CLEAR:
        npipe_android_clear(&core.np);
        seq = 0;
        break;
    case APP_CMD_TIME:
        core_set_utc(&core, cmd->utc);
        if (cmd->has_tz) {
            int16_t tz = cmd->tz_min;
            const cfg_entry_t e = {.key = PROTO_CFG_TIME_TZ_OFFSET_MIN, .val = (const uint8_t *)&tz, .len = 2};
            proto_status_t ks;
            cfg_set(&core.cfg, &e, 1, NULL, &ks); /* little-endian like the wire */
        }
        ui_set_time(core_now_utc(&core));
        break;
    case APP_CMD_PAIRING:
        set_pairing(cmd->on);
        break;
    case APP_CMD_UNBOND:
        if (ble_ok) {
            blec_forget_bonds(); /* BLEC_EV_BONDS(0) follows and opens pairing mode */
        }
        break;
    case APP_CMD_FACE:
#if CONFIG_EPAGER_FEATURE_CLOCK_FACES
        face_command(cmd->title, cmd->body, (uint8_t)cmd->value, cmd->size);
#else
        ESP_LOGI(TAG, "console: no clock faces in this build");
#endif
        break;
    case APP_CMD_CONFIG: {
        const uint8_t v[2] = {(uint8_t)cmd->value, (uint8_t)(cmd->value >> 8)}; /* little-endian, the wire */
        ESP_LOGI(TAG, "console: config %u -> status %d", cmd->key, core_config_set(&core, cmd->key, v, cmd->size));
        break;
    }
#if CONFIG_EPAGER_FEATURE_WIFI
    case APP_CMD_WIFI:
    case APP_CMD_WIFI_FORGET: { /* as WIFI_SET from the app: the core reports the stored name at once */
        const bool forget = cmd->kind == APP_CMD_WIFI_FORGET;
        const proto_status_t st = net_wifi_set(forget ? NULL : (const uint8_t *)cmd->ssid, strlen(cmd->ssid),
                                               (const uint8_t *)cmd->pw, strlen(cmd->pw), forget);
        if (st == PROTO_OK) {
            core_set_wifi_status(&core, forget ? PROTO_WIFI_STATE_UNCONFIGURED : PROTO_WIFI_STATE_IDLE,
                                 forget ? NULL : cmd->ssid);
        }
        ESP_LOGI(TAG, "console: wifi -> status %d", st);
        break;
    }
    case APP_CMD_STATUS: {
        const net_status_t *s = &net_st;
        printf("wifi state %u, network '%s'; weather state %u", s->wifi_state, s->ssid, s->weather_state);
        if (s->have_reading) {
            printf("; %s: %d.%d C, condition %u, %s", s->reading.place, s->reading.temp_c10 / 10,
                   abs(s->reading.temp_c10 % 10), s->reading.cond, s->reading.is_day ? "day" : "night");
        }
        printf("; setup %u; internal RAM %u free, PSRAM %u free\n", s->setup,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        break;
    }
    case APP_CMD_CITY:
        ESP_LOGI(TAG, "console: weather.city -> status %d",
                 core_config_set(&core, PROTO_CFG_WEATHER_CITY, (const uint8_t *)cmd->city, strlen(cmd->city)));
        break;
    case APP_CMD_FETCH:
        net_kick();
        break;
    case APP_CMD_SETUP:
        net_setup(cmd->on);
        break;
    case APP_CMD_UPDATE:
#if CONFIG_EPAGER_FEATURE_WIFI_OTA
        update_command(cmd->title, cmd->body, cmd->value);
#else
        ESP_LOGI(TAG, "console: no Wi-Fi updates in this build");
#endif
        break;
#else
    case APP_CMD_WIFI:
    case APP_CMD_WIFI_FORGET:
    case APP_CMD_STATUS:
    case APP_CMD_CITY:
    case APP_CMD_FETCH:
    case APP_CMD_SETUP:
    case APP_CMD_UPDATE:
        ESP_LOGI(TAG, "console: no Wi-Fi in this build");
        break;
#endif
    }
}

/* ---------------------------------------------------------------- main */

#if CONFIG_EPAGER_DEBUG_CONSOLE
/* PLAN D108/D109: how the main loop spends its passes: a "# loop" line in the battery log every 5 minutes, and the
 * console's `loop` for the time since the last one. *_us: time inside each part of a pass. */
static struct {
    uint32_t passes, radio, tick, pending, idle, max_us, report_ms;
    uint64_t busy_us, io_us, ui_us, pol_us, core_us, log_us, stat_us, clim_us;
} lp;
#define LP_SEC(field)                                                 \
    do {                                                              \
        const int64_t n_ = esp_timer_get_time();                      \
        lp.field += (uint64_t)(n_ - sec_us);                          \
        sec_us = n_;                                                  \
    } while (0)

void app_loop_dump(void)
{
    printf("loop passes=%lu busy_ms=%lu radio=%lu tick=%lu pending=%lu idle=%lu | ms: io=%lu ui=%lu policy=%lu core=%lu log=%lu status=%lu climate=%lu\n",
           (unsigned long)lp.passes, (unsigned long)(lp.busy_us / 1000u), (unsigned long)lp.radio, (unsigned long)lp.tick,
           (unsigned long)lp.pending, (unsigned long)lp.idle, (unsigned long)(lp.io_us / 1000u),
           (unsigned long)(lp.ui_us / 1000u), (unsigned long)(lp.pol_us / 1000u), (unsigned long)(lp.core_us / 1000u),
           (unsigned long)(lp.log_us / 1000u), (unsigned long)(lp.stat_us / 1000u), (unsigned long)(lp.clim_us / 1000u));
}
#else
#define LP_SEC(field) ((void)0)
#endif

void app_run(const app_config_t *cfg)
{
    app = cfg;
    board = cfg->board;
    if (board->init != NULL && !board->init(board->ctx)) {
        ESP_LOGE(TAG, "board init failed");
    }
    const esp_app_desc_t *desc = esp_app_get_description();
    strlcpy(firmware_version, desc->version, sizeof(firmware_version));
    ESP_LOGI(TAG, "%s %s, board %s, IDF %s", cfg->product, firmware_version, board->board_id, desc->idf_ver);

    app_ev_boot();
    power_init();
    power_hold(POWER_LOCK_CPU_MAX); /* released by sleep_policy while idle */
    power_hold(POWER_LOCK_NO_SLEEP);
    main_task = xTaskGetCurrentTaskHandle();
    bool nvs_ok = init_nvs() == ESP_OK;
    bool fs_ok = mount_assets();
    const blec_config_t bc = {.manufacturer = "ePager", .model = board->board_id, .firmware = firmware_version,
                              .wake = ble_wake, .ancs = true};
    ble_ok = nvs_ok && blec_init(&bc);
    bool ui_ok = init_core_and_ui(nvs_ok && app_kv_open(&kv_nvs, "epager"), fs_ok);
    feature_hooks();
    if (ble_ok && ui_ok) {
        refresh_read_values(); /* before the first phone can read them */
        blec_start();
    }
#if CONFIG_EPAGER_FEATURE_OTA
    if (ui_ok) {
        ota_check_passed(OTA_CHECK_UI | (ble_ok ? OTA_CHECK_BLE : 0u));
    }
#endif
    app_batlog_start(board, firmware_version, fs_ok);
    ESP_LOGI(TAG, "boot: nvs %s, assets %s, ble %s, ui %s", nvs_ok ? "ok" : "FAIL", fs_ok ? "ok" : "FAIL",
             ble_ok ? "ok" : "FAIL", ui_ok ? "ok" : "FAIL");
#if CONFIG_EPAGER_FEATURE_WIFI
    if (ui_ok && nvs_ok) { /* the Wi-Fi driver keeps the network in NVS */
        static char ua[48]; /* the update task keeps the pointer */
        snprintf(ua, sizeof(ua), "ePager/%s (%s)", firmware_version, board->board_id);
        net_ok = net_start(ua, now_ms_void);
        sync_net_config();
#if CONFIG_EPAGER_FEATURE_WIFI_OTA
        update_start(ua);
#endif
    }
    ESP_LOGI(TAG, "wifi %s", net_ok ? "ok" : "FAIL");
#endif
    ESP_LOGI(TAG, "heap: %u bytes internal free, %u bytes PSRAM free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (!ui_ok) {
#if CONFIG_EPAGER_FEATURE_OTA
        if (ota_esp_pending_verify()) { /* no loop will tick the verify timeout: go back now */
            ota_esp_rollback();
        }
#endif
        for (;;) {
            vTaskDelay(portMAX_DELAY); /* nothing to show or serve; the watchdog and rollback handle a bad image in M6 */
        }
    }
    app_console_start();

    /* Main loop: keys, console commands, the core tick (clock, battery, flush timers), the clock and
     * sensor for the idle screen, and the UI service. */
    uint32_t last_tick = now_ms(NULL), last_climate = last_tick;
    bool stack_logged = false;
#if CONFIG_EPAGER_FEATURE_OTA
    boot_ms = last_tick;
#endif
#if CONFIG_EPAGER_BOOT_SOUND
    bool boot_sound_done = false;
#endif
    for (;;) {
#if CONFIG_EPAGER_DEBUG_CONSOLE
        const int64_t pass_start = esp_timer_get_time();
        int64_t sec_us = pass_start;
#endif
        key_ev_t ev;
        while (keys != NULL && xQueueReceive(keys, &ev, 0) == pdTRUE) {
            if (ev.btn == EV_WAKE) {
                slp.woke_ms = now_ms(NULL); /* sleep_policy turns the key polling back on */
                continue;
            }
            on_ui_action(NULL, ui_on_button(ev.btn, ev.press), 0);
        }
        app_console_poll();
        const bool radio_busy = ble_service();
#if CONFIG_EPAGER_FEATURE_WIFI
        if (net_ok) {
            apply_net();
        }
#endif
        LP_SEC(io_us);
        bool pending = ui_service();
        LP_SEC(ui_us);
#if CONFIG_EPAGER_BOOT_SOUND
        if (!boot_sound_done) { /* once, after the first picture has been started: the panel refreshes meanwhile */
            boot_sound_done = true;
            if (board->play_audio != NULL) {
                const bool ok = board->play_audio(board->ctx, chime_sample, NULL, board_chime_samples(BOARD_CHIME_RATE_HZ),
                                                  BOARD_CHIME_RATE_HZ);
                ESP_LOGI(TAG, "boot sound %s", ok ? "played" : "FAILED");
#if CONFIG_EPAGER_SOUND_TEST_VOICE
                vTaskDelay(pdMS_TO_TICKS(500));
                ESP_LOGI(TAG, "voice test %s", board->play_audio(board->ctx, voice_sample, NULL,
                                                                 (uint32_t)(voice_pcm_end - voice_pcm_start) / 2,
                                                                 BOARD_CHIME_RATE_HZ) ? "played" : "FAILED");
#endif
#if CONFIG_EPAGER_SOUND_TEST
                vTaskDelay(pdMS_TO_TICKS(500));
                ESP_LOGI(TAG, "speaker test: tones 200 400 800 1600 3200 Hz, sweep 200-4000 Hz, scale");
                ESP_LOGI(TAG, "speaker test %s", board->play_audio(board->ctx, test_sample, NULL,
                                                                   board_test_samples(BOARD_CHIME_RATE_HZ),
                                                                   BOARD_CHIME_RATE_HZ) ? "played" : "FAILED");
#endif
            }
        }
#endif
        uint32_t t = now_ms(NULL);
        /* only the panel's own refresh is left: the chip may sleep through it (PLAN D123) */
        const bool panel_wait = pending && ui_panel_wait() && board->display != NULL && board->display->idle_wake != NULL;
        power_off_poll(t, pending);
        sleep_policy(t, (!pending || panel_wait) && !radio_busy);
        const bool panel_armed = panel_wake_policy(panel_wait);
        LP_SEC(pol_us);
        if (t - last_tick >= CORE_TICK_MS) {
            last_tick = t;
#if CONFIG_EPAGER_FEATURE_OTA
            if (!ota_running_checked && t - boot_ms >= OTA_RUNNING_MS) {
                ota_running_checked = true;
                ota_check_passed(OTA_CHECK_RUNNING);
            }
#endif
            core_tick(&core);
#if CONFIG_EPAGER_FEATURE_WIFI_OTA
            update_tick(t);
#endif
            pairing_timeout(t);
            refresh_read_values_tick(t);
            LP_SEC(core_us);
            app_batlog_tick(t, core_now_utc(&core));
            LP_SEC(log_us);
            sync_ui_status();
            ui_set_time(core_now_utc(&core)); /* redraws only when the minute changes */
            LP_SEC(stat_us);
        }
        if (t - last_climate >= CLIMATE_POLL_MS) {
            last_climate = t;
            poll_climate();
        }
        LP_SEC(clim_us);
        if (!stack_logged && !pending && t > 10000) { /* after the first refresh has finished */
            stack_logged = true;
            ESP_LOGI(TAG, "main task stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        /* A touch panel is polled through LVGL: keep the loop at the input rate when there is one. With the
         * screen off nothing needs the loop but the 1 s tick (and the USB check of D50): wait on the key
         * queue, so a key (or the radio, PLAN D67) wakes it at once. */
        const uint32_t wait_ms = radio_busy ? LOOP_BUSY_MS /* queued notifications, ANCS timers: even with the screen off */
                                 : slp.armed ? (!panel_wait ? CORE_TICK_MS : panel_armed ? LOOP_PANEL_MS : LOOP_BUSY_MS)
                                             : pending || board->read_touch ? LOOP_BUSY_MS : LOOP_IDLE_MS;
#if CONFIG_EPAGER_DEBUG_CONSOLE
        {
            const uint32_t us = (uint32_t)(esp_timer_get_time() - pass_start);
            lp.passes++;
            lp.busy_us += us;
            lp.max_us = us > lp.max_us ? us : lp.max_us;
            if (radio_busy) {
                lp.radio++;
            } else if (slp.armed) {
                lp.tick++;
            } else if (pending || board->read_touch) {
                lp.pending++;
            } else {
                lp.idle++;
            }
            if (t - lp.report_ms >= 300000u) {
                char line[200];
                snprintf(line, sizeof(line),
                         "loop passes=%lu busy_ms=%lu max_ms=%lu radio=%lu tick=%lu pending=%lu idle=%lu io=%lu ui=%lu policy=%lu core=%lu log=%lu status=%lu climate=%lu",
                         (unsigned long)lp.passes, (unsigned long)(lp.busy_us / 1000u), (unsigned long)(lp.max_us / 1000u),
                         (unsigned long)lp.radio, (unsigned long)lp.tick, (unsigned long)lp.pending, (unsigned long)lp.idle,
                         (unsigned long)(lp.io_us / 1000u), (unsigned long)(lp.ui_us / 1000u), (unsigned long)(lp.pol_us / 1000u),
                         (unsigned long)(lp.core_us / 1000u), (unsigned long)(lp.log_us / 1000u),
                         (unsigned long)(lp.stat_us / 1000u), (unsigned long)(lp.clim_us / 1000u));
                app_batlog_note_text(line);
                const uint32_t report = t;
                memset(&lp, 0, sizeof(lp));
                lp.report_ms = report;
            }
        }
#endif
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms)); /* a key, the wake-up interrupt or the radio ends it early */
    }
}
