/* Fake board shim (see fb.h). Built three times by tests/host: libepager_core.so (core on in-memory
 * storage) and, with FB_WITH_SIM, libepager_sim.so (plus the simulated Paper, LVGL and the UI) and,
 * with FB_SIM_LCD as well, libepager_sim_lcd.so (the simulated LCD instead). */
#include "fb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "epager/core.h"

#ifdef FB_WITH_SIM
#include "epager/board_api.h"
#include "epager/ui.h"
#ifdef FB_SIM_LCD
#include "epager/sim_lcd.h"
#define SIM_VARIANT PROTO_VARIANT_LCD
static void sim_reset(void) { sim_lcd_reset(); }
static void sim_tick(uint32_t now_ms) { (void)now_ms; }
static void sim_press(board_button_t b, board_press_t p) { sim_lcd_press(b, p); }
static void sim_set_battery(bool reader, bool readable, uint8_t pct, uint8_t ext)
{
    sim_lcd_set_battery(reader, readable, pct, ext);
}
#else
#include "epager/sim_epaper.h"
#define SIM_VARIANT PROTO_VARIANT_PAPER
static void sim_reset(void) { sim_epaper_reset(); }
static void sim_tick(uint32_t now_ms) { sim_epaper_tick(now_ms); }
static void sim_press(board_button_t b, board_press_t p) { sim_epaper_press(b, p); }
static void sim_set_battery(bool reader, bool readable, uint8_t pct, uint8_t ext)
{
    sim_epaper_set_battery(reader, readable, pct, ext);
}
#endif
#endif

#define QUEUE_CAP 2048u
#define EVENT_CAP 256u
#define REBOOT_DELAY_MS 1000u
#define SELF_CHECK_BIT 1u
#define CLIMATE_POLL_MS 60000u /* as the firmware */

typedef struct {
    uint8_t chr;
    uint16_t len;
    uint8_t v[CORE_MTU_MAX];
} note_t;

static struct {
    bool ready;
    kv_mem_t kv;
    blob_mem_t blob;
    fw_mem_t fw;
    uint8_t *fw_buf;
    npipe_slot_t slots[PROTO_LIMIT_NOTIF_HISTORY_MAX];
    npipe_app_t apps[PROTO_LIMIT_SEEN_APPS_MAX];
    xfer_asset_support_t assets[4];
    core_params_t params;
    core_t core;
    char version[XFER_OTA_VERSION_MAX + 1], prev_version[XFER_OTA_VERSION_MAX + 1], device_name[21];
    bool allow_downgrade;
    uint32_t now;
    bool connected, secure;
    uint8_t battery_sent;      /* last Battery Level notified */
    note_t q[QUEUE_CAP];
    size_t q_head, q_len;
    fb_event_t ev[EVENT_CAP];
    size_t ev_head, ev_len;
    /* battery when there is no simulated board */
    bool bat_reader, bat_readable;
    uint8_t bat_pct, bat_ext;
    /* OTA simulation */
    bool reboot_pending, rollback_pending, self_check_fail;
    uint32_t reboot_at;
    int16_t ota_pct;
    int32_t budget;
    bool powered;
    uint32_t last_climate_ms;
    char wifi_ssid[33], wifi_pw[64]; /* selected profile (RAM only, like the fake storage) */
    char wifi_profiles[3][33];
    uint8_t wifi_profile_count;
    uint8_t bonded;                  /* bonded phones: the Paper's settings page lists them (PLAN D97) */
} fb;

/* ---------------------------------------------------------------- queues */

static void queue_note(uint8_t chr, const uint8_t *v, size_t len)
{
    if (fb.q_len == QUEUE_CAP || len > CORE_MTU_MAX) {
        return; /* the Python side polls after every call; overflow means it stopped */
    }
    note_t *n = &fb.q[(fb.q_head + fb.q_len++) % QUEUE_CAP];
    n->chr = chr;
    n->len = (uint16_t)len;
    memcpy(n->v, v, len);
}

static fb_event_t *new_event(uint32_t type)
{
    if (fb.ev_len == EVENT_CAP) {
        fb.ev_head = (fb.ev_head + 1) % EVENT_CAP; /* drop the oldest */
        fb.ev_len--;
    }
    fb_event_t *e = &fb.ev[(fb.ev_head + fb.ev_len++) % EVENT_CAP];
    memset(e, 0, sizeof(*e));
    e->type = type;
    return e;
}

/* ---------------------------------------------------------------- core callbacks */

static uint32_t cb_now_ms(void *ctx)
{
    (void)ctx;
    return fb.now;
}

static bool cb_battery(void *ctx, uint8_t *pct, uint8_t *ext)
{
    (void)ctx;
    *pct = fb.bat_pct;
    *ext = fb.bat_ext;
    return fb.bat_readable;
}

static void cb_notify(void *ctx, uint8_t chr, const uint8_t *val, size_t len)
{
    (void)ctx;
    queue_note(chr, val, len);
}

#ifdef FB_WITH_SIM
static void sync_ui_config(void)
{
    ui_config_t c = {.language = (uint8_t)cfg_get_int(&fb.core.cfg, PROTO_CFG_UI_LANGUAGE),
                     .show_body = cfg_get_int(&fb.core.cfg, PROTO_CFG_DISPLAY_SHOW_BODY) != 0,
                     .mask_numbers = cfg_get_int(&fb.core.cfg, PROTO_CFG_DISPLAY_MASK_NUMBERS) != 0,
                     .full_refresh_every = (uint8_t)cfg_get_int(&fb.core.cfg, PROTO_CFG_DISPLAY_FULL_REFRESH_EVERY),
                     .tz_offset_min = (int16_t)cfg_get_int(&fb.core.cfg, PROTO_CFG_TIME_TZ_OFFSET_MIN),
                     .clock_24h = cfg_get_int(&fb.core.cfg, PROTO_CFG_UI_CLOCK_24H) != 0,
                     .temp_unit = (uint8_t)cfg_get_int(&fb.core.cfg, PROTO_CFG_UI_TEMP_UNIT),
                     .brightness = (uint8_t)cfg_get_int(&fb.core.cfg, PROTO_CFG_DISPLAY_BRIGHTNESS),
                     .idle_dim_s = (uint16_t)cfg_get_int(&fb.core.cfg, PROTO_CFG_DISPLAY_IDLE_DIM_S),
                     .idle_off_s = (uint16_t)cfg_get_int(&fb.core.cfg, PROTO_CFG_DISPLAY_IDLE_OFF_S),
                     .orientation = (uint8_t)cfg_get_int(&fb.core.cfg, PROTO_CFG_DISPLAY_ORIENTATION),
                     .weather_enabled = cfg_get_int(&fb.core.cfg, PROTO_CFG_WEATHER_ENABLED) != 0,
                     .update_auto_check = cfg_get_int(&fb.core.cfg, PROTO_CFG_UPDATE_AUTO_CHECK) != 0};
    ui_set_config(&c);
}

static void poll_climate(void)
{
    const board_profile_t *b = board_get();
    board_climate_t c;
    fb.last_climate_ms = fb.now;
    if (b->read_climate) {
        ui_set_climate(b->read_climate(b->ctx, &c) ? &c : NULL);
    }
}

static int32_t fb_passkey = -1; /* -1: no pairing in progress */

static void sync_ui_status(void)
{
    ui_status_t s = {.has_passkey = fb_passkey >= 0, .passkey = fb_passkey >= 0 ? (uint32_t)fb_passkey : 0, .battery_pct = fb.core.status.battery_pct, .wifi_state = fb.core.status.wifi_state,
                     .external_power = fb.core.status.external_power == PROTO_EXTERNAL_POWER_YES,
                     .connected = fb.connected, .pairing_mode = fb.core.status.pairing_mode, .ota_pct = fb.ota_pct};
    ui_set_status(&s);
}

/* What the settings pages list (PLAN D97): the firmware does the same from the BLE glue and the Wi-Fi task. */
static void refresh_info(void)
{
    ui_info_t i;
    memset(&i, 0, sizeof(i));
    i.n_phones = fb.bonded > UI_PHONES_MAX ? (uint8_t)UI_PHONES_MAX : fb.bonded;
    i.phone_connected[0] = fb.connected && i.n_phones > 0;
    i.n_networks = fb.wifi_profile_count;
    memcpy(i.networks, fb.wifi_profiles, sizeof(i.networks));
    snprintf(i.firmware, sizeof(i.firmware), "%.31s", fb.version);
    snprintf(i.device, sizeof(i.device), "%s", fb.device_name);
    snprintf(i.board, sizeof(i.board), "%s", "fake_epaper_154");
    ui_set_info(&i);
}
#else
static void refresh_info(void) {}
#endif

static void cb_notif(void *ctx, const npipe_notif_t *n)
{
    (void)ctx;
    fb_event_t *e = new_event(FB_EV_NOTIF);
    e->a = n->kind;
    e->id = n->id;
    e->timestamp = n->timestamp;
    e->source = n->source;
    e->category = n->category;
    e->priority = n->priority;
    e->flags = n->flags;
    e->shown = n->shown;
    memcpy(e->app_id, n->app_id, sizeof(e->app_id));
    memcpy(e->app_name, n->app_name, sizeof(e->app_name));
    memcpy(e->title, n->title, sizeof(e->title));
    memcpy(e->subtitle, n->subtitle, sizeof(e->subtitle));
    memcpy(e->body, n->body, sizeof(e->body));
#ifdef FB_WITH_SIM
    ui_on_notification(n);
#endif
}

static proto_status_t cb_wifi_set(void *ctx, const uint8_t *ssid, size_t ssid_len, const uint8_t *pw, size_t pw_len,
                                  bool forget)
{
    (void)ctx;
    char name[33] = "";
    if (ssid) {
        snprintf(name, sizeof(name), "%.*s", (int)ssid_len, (const char *)ssid);
    }
    size_t at = fb.wifi_profile_count;
    for (size_t i = 0; i < fb.wifi_profile_count; i++) {
        if (strcmp(fb.wifi_profiles[i], name) == 0) {
            at = i;
            break;
        }
    }
    if (forget) {
        if (!ssid) {
            fb.wifi_profile_count = 0;
            fb.wifi_ssid[0] = '\0';
            fb.wifi_pw[0] = '\0';
        } else if (at < fb.wifi_profile_count) {
            memmove(&fb.wifi_profiles[at], &fb.wifi_profiles[at + 1],
                    (fb.wifi_profile_count - at - 1) * sizeof(fb.wifi_profiles[0]));
            fb.wifi_profile_count--;
            fb.wifi_profiles[fb.wifi_profile_count][0] = '\0';
        }
    } else {
        if (at < fb.wifi_profile_count) {
            memmove(&fb.wifi_profiles[at], &fb.wifi_profiles[at + 1],
                    (fb.wifi_profile_count - at - 1) * sizeof(fb.wifi_profiles[0]));
            fb.wifi_profile_count--;
        } else if (fb.wifi_profile_count == 3) {
            memmove(&fb.wifi_profiles[0], &fb.wifi_profiles[1], 2 * sizeof(fb.wifi_profiles[0]));
            fb.wifi_profile_count--;
        }
        snprintf(fb.wifi_profiles[fb.wifi_profile_count++], sizeof(fb.wifi_profiles[0]), "%s", name);
        snprintf(fb.wifi_ssid, sizeof(fb.wifi_ssid), "%s", name);
        snprintf(fb.wifi_pw, sizeof(fb.wifi_pw), "%.*s", (int)pw_len, pw ? (const char *)pw : "");
    }
    refresh_info();
    return PROTO_OK;
}

static void cb_asset(void *ctx, uint8_t asset_type)
{
    (void)ctx;
#ifdef FB_WITH_SIM
    ui_assets_changed(asset_type);
#else
    (void)asset_type;
#endif
}

static void cb_config(void *ctx, const uint16_t *keys, size_t n)
{
    (void)ctx;
    (void)keys;
    fb_event_t *e = new_event(FB_EV_CONFIG);
    e->a = (uint32_t)n;
    e->b = cfg_revision(&fb.core.cfg);
#ifdef FB_WITH_SIM
    sync_ui_config();
#endif
}

static void cb_setting(void *ctx, core_setting_t kind)
{
    (void)ctx;
#ifdef FB_WITH_SIM
    ui_show_notice(kind == CORE_SETTING_WIFI ? UI_NOTICE_WIFI : UI_NOTICE_SETTING);
#else
    (void)kind;
#endif
}

static void cb_ota(void *ctx, const xfer_ota_event_t *ev)
{
    (void)ctx;
    fb_event_t *e = new_event(FB_EV_OTA);
    e->a = ev->kind;
    e->b = ev->pct;
    e->c = ev->state;
    switch (ev->kind) {
    case XFER_OTA_EV_PROGRESS:
        fb.ota_pct = ev->pct;
        break;
    case XFER_OTA_EV_REBOOT:
        fb.reboot_pending = true;
        fb.reboot_at = fb.now + REBOOT_DELAY_MS;
        break;
    case XFER_OTA_EV_ROLLBACK:
        fb.rollback_pending = true;
        fb.reboot_pending = true;
        fb.reboot_at = fb.now + REBOOT_DELAY_MS;
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- boot */

#ifdef FB_WITH_SIM
static void on_button(void *user, board_button_t btn, board_press_t press);
static void on_ui_action(void *ctx, ui_action_t a, uint32_t arg);
#endif

static void boot(bool image_pending_verify)
{
    memset(fb.slots, 0, sizeof(fb.slots));
    memset(fb.apps, 0, sizeof(fb.apps));
    fb.assets[0] = (xfer_asset_support_t){PROTO_ASSET_TYPE_BACKGROUND, PROTO_ASSET_FORMAT_MONO1, 5016, 0, 0};
    fb.assets[1] = (xfer_asset_support_t){PROTO_ASSET_TYPE_THEME, PROTO_ASSET_FORMAT_THEME_V1, 4096, 0, 0};
    fb.assets[2] = (xfer_asset_support_t){PROTO_ASSET_TYPE_FONT, PROTO_ASSET_FORMAT_FONT_V1, 262144, 0, 0};
    fb.assets[3] = (xfer_asset_support_t){PROTO_ASSET_TYPE_APP_ICON, PROTO_ASSET_FORMAT_MONO1, 88, 24, 24};
    fb.params = (core_params_t){
        .device_name = fb.device_name, .board_id = "fake_epaper_154", .firmware_version = fb.version,
        .variant = PROTO_VARIANT_PAPER, .display_type = PROTO_DISPLAY_TYPE_EPAPER_BW, .color_depth = 1,
        .refresh_class = PROTO_REFRESH_CLASS_EPAPER, .display_w = 200, .display_h = 200,
        .features = PROTO_FEATURES_ANCS | PROTO_FEATURES_ANDROID_NOTIFICATIONS | PROTO_FEATURES_ASSETS |
                    PROTO_FEATURES_OTA | PROTO_FEATURES_BATTERY,
        .transports = PROTO_TRANSPORTS_BLE, .max_mtu = 517,
        .kv_cfg = &fb.kv.iface, .kv_apps = &fb.kv.iface, .kv_xfer = &fb.kv.iface,
        .blob = &fb.blob.iface, .assets = fb.assets, .n_assets = 4, .fw = &fb.fw.iface,
        .allow_downgrade = fb.allow_downgrade, .low_battery_pct = 30,
        .ota_required_checks = SELF_CHECK_BIT, .ota_verify_timeout_ms = 30000,
        .ack_window = 4096, .journal_every = 16384,
        .slots = fb.slots, .n_slots = PROTO_LIMIT_NOTIF_HISTORY_MAX, .apps = fb.apps,
        .n_apps = PROTO_LIMIT_SEEN_APPS_MAX,
        .now_ms = cb_now_ms, .read_battery = fb.bat_reader ? cb_battery : NULL, .battery_poll_ms = 1000,
        .apps_flush_ms = 60000,
        .notify = cb_notify, .on_notif = cb_notif, .on_config = cb_config, .on_setting = cb_setting, .on_ota = cb_ota,
        .on_asset = cb_asset, .wifi_set = cb_wifi_set, .ctx = NULL};
#ifdef FB_WITH_SIM
    /* identity, display and battery come from the simulated board through board_api */
    const board_profile_t *b = board_get();
    board_display_info_t di;
    b->display->get_info(b->display->ctx, &di);
    fb.params.board_id = b->board_id;
    fb.params.features = b->features;
    fb.params.low_battery_pct = b->low_battery_ota_pct;
    fb.params.display_type = di.display_type;
    fb.params.color_depth = di.color_depth;
    fb.params.refresh_class = di.refresh_class;
    fb.params.display_w = di.width;
    fb.params.display_h = di.height;
    fb.params.variant = SIM_VARIANT;
#ifdef FB_SIM_LCD
    fb.assets[0] = (xfer_asset_support_t){PROTO_ASSET_TYPE_BACKGROUND, PROTO_ASSET_FORMAT_RGB565, 142096, 0, 0};
    fb.assets[1] = (xfer_asset_support_t){PROTO_ASSET_TYPE_APP_ICON, PROTO_ASSET_FORMAT_RGB565A8, 3088, 32, 32};
    fb.params.n_assets = 2;
#endif
    fb.params.read_battery = b->read_battery;
    fb.params.rtc_get = b->rtc_get;
    fb.params.rtc_set = b->rtc_set;
    fb.params.ctx = b->ctx;
    /* the callbacks above ignore ctx; the battery reader and the RTC get the board's */
#endif
    fb.ota_pct = -1;
    fb.reboot_pending = fb.rollback_pending = false;
    fb.powered = true;
    core_init(&fb.core, &fb.params);
    if (fb.params.features & PROTO_FEATURES_WIFI) { /* what the firmware reports at boot */
        core_set_wifi_status(&fb.core, fb.wifi_ssid[0] ? PROTO_WIFI_STATE_IDLE : PROTO_WIFI_STATE_UNCONFIGURED,
                             fb.wifi_ssid);
        core_set_weather_state(&fb.core, PROTO_WEATHER_STATE_WAITING);
    }
#ifdef FB_WITH_SIM
    ui_params_t up = {.display = b->display, .blob = &fb.blob.iface, .climate = b->read_climate != NULL,
                      .now_ms = cb_now_ms, .read_touch = b->read_touch, .touch_ctx = b->ctx,
                      .on_action = on_ui_action,
                      .cards_timeout_s = UI_CARDS_TIMEOUT_S,
                      .wifi_setup = (fb.params.features & PROTO_FEATURES_WIFI) != 0,
                     .update = (fb.params.features & PROTO_FEATURES_WIFI) != 0};
    ui_init(&up);
    if (b->set_button_handler) {
        b->set_button_handler(b->ctx, on_button, NULL);
    }
    sync_ui_config();
    sync_ui_status();
    ui_set_time(core_now_utc(&fb.core));
    poll_climate();
#endif
    core_boot(&fb.core, image_pending_verify);
    if (image_pending_verify) {
        if (fb.self_check_fail) {
            xfer_ota_check_failed(&fb.core.ota);
        } else {
            xfer_ota_check_passed(&fb.core.ota, SELF_CHECK_BIT);
        }
    }
    fb_event_t *e = new_event(FB_EV_REBOOT);
    e->a = image_pending_verify;
    snprintf(e->text, sizeof(e->text), "%s", fb.version);
}

/* graceful: the firmware closes the link itself (journals the transfer offset); otherwise the
 * board resets or loses power and RAM state is simply gone. */
static void drop_link(bool graceful)
{
    if (fb.connected) {
        if (graceful) {
            core_disconnect(&fb.core);
        }
        fb.connected = false;
        new_event(FB_EV_DISCONNECT);
    }
}

/* The firmware reboot the OTA flow asked for: into the new image, or back after a rollback. */
static void ota_reboot(void)
{
    drop_link(true);
    bool pending = false;
    if (fb.rollback_pending) {
        snprintf(fb.version, sizeof(fb.version), "%s", fb.prev_version);
    } else if (fb.fw.boot_set) {
        uint8_t rec[3 + XFER_OTA_VERSION_MAX];
        size_t n = 0;
        if (fb.kv.iface.get(fb.kv.iface.ctx, XFER_OTA_KV_KEY, rec, sizeof(rec), &n) == KV_OK && n >= 3 &&
            rec[2] == n - 3) {
            snprintf(fb.prev_version, sizeof(fb.prev_version), "%s", fb.version);
            memcpy(fb.version, rec + 3, rec[2]);
            fb.version[rec[2]] = 0;
            pending = true;
        }
        fb.fw.boot_set = false;
    }
    boot(pending);
}

/* ---------------------------------------------------------------- API */

size_t fb_event_size(void) { return sizeof(fb_event_t); }
size_t fb_display_size(void) { return sizeof(fb_display_t); }

int fb_has_screen(void)
{
#ifdef FB_WITH_SIM
    return 1;
#else
    return 0;
#endif
}

int fb_init(uint32_t storage_total, uint32_t fw_slot, const char *version, int allow_downgrade,
            const char *device_name)
{
    if (fb.ready) {
        blob_mem_free(&fb.blob);
        free(fb.fw_buf);
    }
    memset(&fb, 0, sizeof(fb));
    kv_mem_init(&fb.kv);
    blob_mem_init(&fb.blob, storage_total);
    fb.fw_buf = calloc(1, fw_slot ? fw_slot : 1);
    if (!fb.fw_buf) {
        return -1;
    }
    fw_mem_init(&fb.fw, fb.fw_buf, fw_slot);
    snprintf(fb.version, sizeof(fb.version), "%s", version);
    snprintf(fb.prev_version, sizeof(fb.prev_version), "%s", version);
    snprintf(fb.device_name, sizeof(fb.device_name), "%s", device_name);
    fb.allow_downgrade = allow_downgrade != 0;
    fb.bat_reader = fb.bat_readable = true;
    fb.bat_pct = 80;
    fb.bat_ext = PROTO_EXTERNAL_POWER_NO;
    fb.budget = -1;
    fb.ready = true;
#ifdef FB_WITH_SIM
    sim_reset();
#endif
    boot(false);
    return 0;
}

void fb_reboot(void)
{
    drop_link(false);
    boot(false);
}

void fb_set_time(uint32_t now_ms) { fb.now = now_ms; }

void fb_tick(void)
{
    if (fb.reboot_pending && (int32_t)(fb.now - fb.reboot_at) >= 0) {
        ota_reboot();
    }
    if (!fb.powered) {
#ifdef FB_WITH_SIM
        sim_tick(fb.now);
        (void)ui_service();
#endif
        return;
    }
    core_tick(&fb.core);
    uint8_t pct = core_battery_pct(&fb.core);
    if (fb.connected && fb.secure && pct != fb.battery_sent && pct <= 100) {
        fb.battery_sent = pct;
        queue_note(FB_CHR_BATTERY, &pct, 1);
    }
#ifdef FB_WITH_SIM
    sim_tick(fb.now);
    sync_ui_status();
    ui_set_time(core_now_utc(&fb.core));
    if (fb.now - fb.last_climate_ms >= CLIMATE_POLL_MS) {
        poll_climate();
    }
    (void)ui_service();
#endif
}

void fb_connect(uint16_t mtu, int secure)
{
    if (!fb.powered) {
        return;
    }
    fb.connected = true;
    fb.secure = secure != 0;
    fb.battery_sent = core_battery_pct(&fb.core);
    core_connect(&fb.core, mtu, fb.secure);
}

void fb_disconnect(void)
{
    if (fb.connected) {
        core_disconnect(&fb.core);
        fb.connected = false;
    }
}

void fb_set_secure(int secure)
{
    fb.secure = secure != 0;
    core_set_secure(&fb.core, fb.secure);
}

int fb_connected(void) { return fb.connected; }

void fb_write(uint8_t chr, const uint8_t *data, size_t len)
{
    if (fb.connected) {
        core_write(&fb.core, chr, data, len);
    }
}

size_t fb_read(uint8_t chr, uint8_t *out, size_t cap)
{
    if (chr == FB_CHR_BATTERY) {
        uint8_t pct = core_battery_pct(&fb.core);
        if (!fb.secure || pct > 100 || cap < 1) {
            return 0;
        }
        out[0] = pct;
        return 1;
    }
    return core_read(&fb.core, chr, out, cap);
}

int fb_poll(uint8_t *chr, uint8_t *out, size_t cap)
{
    if (!fb.q_len) {
        return -1;
    }
    note_t *n = &fb.q[fb.q_head];
    fb.q_head = (fb.q_head + 1) % QUEUE_CAP;
    fb.q_len--;
    size_t len = n->len < cap ? n->len : cap;
    memcpy(out, n->v, len);
    *chr = n->chr;
    return (int)len;
}

int fb_poll_event(fb_event_t *out)
{
    if (!fb.ev_len) {
        return 0;
    }
    *out = fb.ev[fb.ev_head];
    fb.ev_head = (fb.ev_head + 1) % EVENT_CAP;
    fb.ev_len--;
    return 1;
}

void fb_set_battery(int reader, int readable, uint8_t pct, uint8_t external_power)
{
    fb.bat_reader = reader != 0;
    fb.bat_readable = readable != 0;
    fb.bat_pct = pct;
    fb.bat_ext = external_power;
#ifdef FB_WITH_SIM
    sim_set_battery(reader != 0, readable != 0, pct, external_power);
#endif
    core_poll_battery(&fb.core);
}

void fb_set_utc(uint32_t utc_s) { core_set_utc(&fb.core, utc_s); }
#if defined(FB_WITH_SIM) && !defined(FB_SIM_LCD) /* the LCD has no RTC and no sensor */
void fb_set_rtc(int present, uint32_t utc_s) { sim_epaper_set_rtc(present != 0, utc_s); }
uint32_t fb_rtc_utc(void) { return sim_epaper_rtc(); }

void fb_set_climate(int present, int readable, int16_t centi_c, uint16_t centi_rh)
{
    sim_epaper_set_climate(present != 0, readable != 0, centi_c, centi_rh);
    if (fb.powered) {
        poll_climate(); /* shown now instead of at the next minute's reading */
    }
}
#else
void fb_set_rtc(int present, uint32_t utc_s)
{
    (void)present;
    (void)utc_s;
}
uint32_t fb_rtc_utc(void) { return 0; }
void fb_set_climate(int present, int readable, int16_t centi_c, uint16_t centi_rh)
{
    (void)present;
    (void)readable;
    (void)centi_c;
    (void)centi_rh;
}
#endif
void fb_set_pairing(int on) { core_set_pairing_mode(&fb.core, on != 0); }
/* The passkey a pairing phone asks for, shown by the UI (as the firmware shows it); < 0 removes it. */
void fb_set_passkey(int32_t passkey)
{
#ifdef FB_WITH_SIM
    fb_passkey = passkey;
    sync_ui_status();
#else
    (void)passkey;
#endif
}
void fb_set_bonded(uint8_t n)
{
    fb.bonded = n;
    core_set_bonded_count(&fb.core, n);
    refresh_info();
}

void fb_set_storage_total(uint32_t total)
{
    uint32_t t, free_bytes;
    fb.blob.iface.usage(fb.blob.iface.ctx, &t, &free_bytes);
    uint32_t used = t - free_bytes;
    fb.blob.total = total < used ? used : total;
}

void fb_storage_budget(int32_t writes)
{
    fb.budget = writes;
    int32_t *p = writes < 0 ? NULL : &fb.budget;
    fb.kv.power = fb.blob.power = fb.fw.power = p;
}

int fb_corrupt_transfer(void)
{
    for (size_t i = 0; i < BLOB_MEM_MAX_FILES; i++) {
        blob_mem_file_t *f = &fb.blob.f[i];
        size_t n = strlen(f->name);
        if (f->used && n && f->name[n - 1] == '~' && f->size) {
            f->data[f->size - 1] ^= 0xFFu;
            return 1;
        }
    }
    if (fb.core.xfer.has_session && fb.core.xfer.s.t.kind == PROTO_TRANSFER_KIND_FIRMWARE && fb.fw.written) {
        fb.fw_buf[fb.fw.written - 1] ^= 0xFFu;
        return 1;
    }
    return 0;
}

void fb_set_fw_image_status(uint8_t status) { fb.fw.image_status = (proto_status_t)status; }
void fb_set_self_check_fail(int fail) { fb.self_check_fail = fail != 0; }

static npipe_str_t str(const char *s) { return (npipe_str_t){(const uint8_t *)s, s ? strlen(s) : 0}; }

void fb_ancs(uint8_t event_id, uint8_t event_flags, uint8_t category, uint32_t uid, const char *app_id,
             const char *app_name, const char *title, const char *subtitle, const char *message, const char *date)
{
    npipe_ancs_t in = {.ns = {.event_id = event_id, .event_flags = event_flags, .category_id = category,
                              .category_count = 1, .uid = uid},
                       .app_id = str(app_id), .app_name = str(app_name), .title = str(title),
                       .subtitle = str(subtitle), .message = str(message), .date = str(date)};
    if (event_id != 2 && (in.app_id.len == 0 || in.app_id.len > NPIPE_APP_ID_MAX)) {
        return; /* the ANCS client never forwards an added/modified event without an app id */
    }
    npipe_ancs_event(&fb.core.np, &in);
}

void fb_ancs_reset(void) { npipe_ancs_session_reset(&fb.core.np); }

int fb_android(const char *key, const char *app_id, const char *app_name, const char *title, const char *subtitle,
               const char *body, uint8_t category, uint8_t priority, uint8_t flags, uint32_t *id)
{
    npipe_android_t in = {.key = str(key), .app_id = str(app_id), .app_name = str(app_name), .title = str(title),
                          .subtitle = str(subtitle), .body = str(body), .category = category, .priority = priority,
                          .flags = flags};
    /* the same limits message validation enforces before npipe sees a NOTIF_POST */
    if (!in.key.len || in.key.len > NPIPE_KEY_MAX || !in.app_id.len || in.app_id.len > NPIPE_APP_ID_MAX ||
        !proto_utf8_valid(in.key.p, in.key.len) || !proto_utf8_valid(in.app_id.p, in.app_id.len) ||
        in.app_name.len > NPIPE_APP_NAME_MAX || in.title.len > NPIPE_TITLE_MAX ||
        in.subtitle.len > NPIPE_SUBTITLE_MAX || in.body.len > NPIPE_BODY_MAX) {
        return PROTO_TOO_LARGE;
    }
    bool shown;
    uint32_t nid = 0;
    proto_status_t st = npipe_android_post(&fb.core.np, &in, &nid, &shown);
    if (id) {
        *id = nid;
    }
    return st;
}

int fb_android_remove(const char *key) { return npipe_android_remove(&fb.core.np, (const uint8_t *)key, strlen(key)); }

size_t fb_wifi_network(char *ssid, size_t cap, char *password, size_t pw_cap)
{
    snprintf(ssid, cap, "%s", fb.wifi_ssid);
    snprintf(password, pw_cap, "%s", fb.wifi_pw);
    return strlen(fb.wifi_ssid);
}

void fb_set_wifi_state(uint8_t state) { core_set_wifi_status(&fb.core, state, fb.wifi_ssid); }
void fb_set_weather_state(uint8_t state) { core_set_weather_state(&fb.core, state); }

#ifdef FB_WITH_SIM
void fb_weather(int present, int16_t temp_c10, int32_t wmo_code, int is_day, int16_t high_c10, int16_t low_c10,
                uint8_t precip_pct, const char *place)
{
    if (!present) {
        ui_set_weather(NULL);
        return;
    }
    wx_reading_t r = {.temp_c10 = temp_c10, .high_c10 = high_c10, .low_c10 = low_c10, .precip_pct = precip_pct,
                      .cond = (uint8_t)wx_cond_from_wmo(wmo_code), .is_day = is_day != 0};
    snprintf(r.place, sizeof(r.place), "%s", place ? place : "");
    ui_set_weather(&r);
}

int fb_weather_json(const char *json, size_t len, const char *place)
{
    wx_reading_t r;
    wx_result_t res = wx_parse_forecast(json, len, &r);
    if (res == WX_OK) {
        snprintf(r.place, sizeof(r.place), "%s", place ? place : ""); /* the place comes from the geocoder, as on the board */
        ui_set_weather(&r);
    }
    return (int)res;
}

static void set_setup(ui_setup_state_t state, const char *ap_ssid, const char *ap_password, const char *url,
                      const char *network)
{
    ui_wifi_setup_t s = {.state = state};
    snprintf(s.ap_ssid, sizeof(s.ap_ssid), "%s", ap_ssid);
    snprintf(s.ap_password, sizeof(s.ap_password), "%s", ap_password);
    snprintf(s.url, sizeof(s.url), "%s", url);
    snprintf(s.network, sizeof(s.network), "%s", network);
    ui_set_wifi_setup(&s);
}

void fb_wifi_setup(uint8_t state, const char *ap_ssid, const char *ap_password, const char *url, const char *network)
{
    set_setup((ui_setup_state_t)state, ap_ssid, ap_password, url, network);
}

void fb_set_update(uint8_t state, uint8_t error, const char *version, uint32_t size, int skipped)
{
    ui_update_t u = {.state = (ui_update_state_t)state, .error = (ui_update_error_t)error, .size = size,
                     .skipped = skipped != 0};
    snprintf(u.version, sizeof(u.version), "%s", version ? version : "");
    u.mark = state == UI_UPD_AVAILABLE && !u.skipped;
    ui_set_update(&u);
}

int fb_update_prompt(void) { return ui_show_update_prompt() ? 1 : 0; }
#else
void fb_set_update(uint8_t state, uint8_t error, const char *version, uint32_t size, int skipped)
{
    (void)state, (void)error, (void)version, (void)size, (void)skipped;
}
int fb_update_prompt(void) { return 0; }
void fb_weather(int present, int16_t temp_c10, int32_t wmo_code, int is_day, int16_t high_c10, int16_t low_c10,
                uint8_t precip_pct, const char *place)
{
    (void)present, (void)temp_c10, (void)wmo_code, (void)is_day, (void)high_c10, (void)low_c10, (void)precip_pct;
    (void)place;
}
int fb_weather_json(const char *json, size_t len, const char *place)
{
    (void)json;
    (void)len;
    (void)place;
    return -1;
}
void fb_wifi_setup(uint8_t state, const char *ap_ssid, const char *ap_password, const char *url, const char *network)
{
    (void)state, (void)ap_ssid, (void)ap_password, (void)url, (void)network;
}
#endif

int32_t fb_config_int(uint16_t key) { return cfg_get_int(&fb.core.cfg, key); }
uint32_t fb_config_revision(void) { return cfg_revision(&fb.core.cfg); }
uint32_t fb_notif_count(int shown_only) { return (uint32_t)npipe_count(&fb.core.np, shown_only != 0); }
uint8_t fb_ota_state(void) { return fb.core.status.ota_state; }
uint8_t fb_battery_pct(void) { return core_battery_pct(&fb.core); }
uint32_t fb_now_utc(void) { return core_now_utc(&fb.core); }

size_t fb_version(char *out, size_t cap) { return (size_t)snprintf(out, cap, "%s", fb.version); }

/* ---------------------------------------------------------------- screen */

#ifdef FB_WITH_SIM
/* What the firmware does with a UI action (keys and touch alike). */
static void on_ui_action(void *ctx, ui_action_t a, uint32_t arg)
{
    (void)ctx;
    if (a == UI_ACTION_NONE) {
        return;
    }
    fb_event_t *e = new_event(FB_EV_ACTION);
    e->a = a;
    e->b = arg;
    switch (a) {
    case UI_ACTION_PAIRING_TOGGLE:
        core_set_pairing_mode(&fb.core, !fb.core.status.pairing_mode);
        break;
    case UI_ACTION_DISMISS:
        core_dismiss(&fb.core, arg);
        break;
    case UI_ACTION_WIFI_SETUP: /* no radio here: the screen as the firmware shows it, a fixed password */
        set_setup(UI_SETUP_WAITING, fb.device_name, "fake-pass1", "192.168.4.1", "");
        break;
    case UI_ACTION_WIFI_SETUP_CLOSE:
        set_setup(UI_SETUP_OFF, "", "", "", "");
        break;
    case UI_ACTION_INFO_REFRESH:
        refresh_info();
        break;
    case UI_ACTION_FORGET_PHONE:
        if (fb.bonded > 0) {
            fb.bonded--;
            core_set_bonded_count(&fb.core, fb.bonded);
        }
        refresh_info();
        break;
    case UI_ACTION_FORGET_NETWORK:
        if (arg < fb.wifi_profile_count) {
            char name[33];
            snprintf(name, sizeof(name), "%s", fb.wifi_profiles[arg]);
            (void)cb_wifi_set(NULL, (const uint8_t *)name, strlen(name), NULL, 0, true);
            core_set_wifi_status(&fb.core, fb.wifi_profile_count ? PROTO_WIFI_STATE_IDLE : PROTO_WIFI_STATE_UNCONFIGURED,
                                 fb.wifi_ssid);
        }
        break;
    case UI_ACTION_SETTING: /* the Paper's settings list: a config key and its new value (PLAN D96) */
        core_config_set_u8(&fb.core, (uint16_t)(arg >> 8), (uint8_t)arg);
        break;
    case UI_ACTION_BRIGHTNESS_UP:
    case UI_ACTION_BRIGHTNESS_DOWN: {
        const uint8_t now = (uint8_t)cfg_get_int(&fb.core.cfg, PROTO_CFG_DISPLAY_BRIGHTNESS);
        core_config_set_u8(&fb.core, PROTO_CFG_DISPLAY_BRIGHTNESS,
                           ui_brightness_step(now, a == UI_ACTION_BRIGHTNESS_UP));
        break;
    }
    default:
        break;
    }
}

static void on_button(void *user, board_button_t btn, board_press_t press)
{
    (void)user;
    ui_action_t a = ui_on_button(btn, press);
    if (a != UI_ACTION_POWER_TOGGLE) {
        on_ui_action(NULL, a, 0);
        return;
    }
    new_event(FB_EV_ACTION)->a = a;
    if (fb.powered) { /* power off: the link drops, the panel keeps its last image */
        drop_link(true);
        fb.powered = false;
        ui_set_power(false);
        new_event(FB_EV_POWER)->a = 0;
    } else {
        boot(false);
        new_event(FB_EV_POWER)->a = 1;
    }
}

#ifdef FB_SIM_LCD
void fb_screen_info(uint16_t *w, uint16_t *h, uint8_t *channels)
{
    *w = SIM_LCD_W;
    *h = SIM_LCD_H;
    *channels = 3;
}
void fb_panel(uint8_t *px) { sim_lcd_panel(px); }
void fb_frame(uint8_t *frame) { memcpy(frame, sim_lcd_frame(), SIM_LCD_W * SIM_LCD_H * 2u); }
void fb_touch(int pressed, uint16_t x, uint16_t y) { sim_lcd_touch(pressed != 0, x, y); }

void fb_display_state(fb_display_t *out)
{
    sim_lcd_state_t s;
    sim_lcd_state(&s);
    *out = (fb_display_t){.asleep = s.asleep, .violations = s.violations, .powered = fb.powered,
                          .link = ui_link_icon(), .backlight = s.backlight,
                          .wifi = ui_wifi_icon()};
}

void fb_set_ghosting(int on) { (void)on; }
void fb_set_refresh_timing(uint32_t partial_ms, uint32_t full_ms)
{
    (void)partial_ms;
    (void)full_ms;
}
#else
void fb_screen_info(uint16_t *w, uint16_t *h, uint8_t *channels)
{
    *w = SIM_EPAPER_W;
    *h = SIM_EPAPER_H;
    *channels = 1;
}
void fb_panel(uint8_t *px) { sim_epaper_panel(px); }
void fb_frame(uint8_t *frame) { memcpy(frame, sim_epaper_frame(), SIM_EPAPER_FRAME); }
void fb_touch(int pressed, uint16_t x, uint16_t y)
{
    (void)pressed;
    (void)x;
    (void)y;
}

void fb_display_state(fb_display_t *out)
{
    sim_epaper_state_t s;
    ui_stats_t u;
    sim_epaper_state(&s);
    ui_get_stats(&u);
    *out = (fb_display_t){.refreshing = s.refreshing, .asleep = s.asleep, .kind = s.kind, .phase = s.phase,
                          .elapsed_ms = s.elapsed_ms, .partial = s.partial, .full = s.full,
                          .violations = s.violations, .ui_partial = u.partial, .ui_full = u.full,
                          .ui_skipped = u.skipped, .partials_since_full = u.partials_since_full,
                          .powered = fb.powered, .link = ui_link_icon(), .wifi = ui_wifi_icon()};
}

void fb_set_ghosting(int on) { sim_epaper_set_ghosting(on != 0); }
void fb_set_refresh_timing(uint32_t partial_ms, uint32_t full_ms) { sim_epaper_set_timing(partial_ms, full_ms); }
#endif
void fb_button(uint8_t btn, int press) /* 0 short, 1 long, 2 hold (PWR 5 s) */
{
    sim_press((board_button_t)btn, press == 2 ? BOARD_PRESS_HOLD : press ? BOARD_PRESS_LONG : BOARD_PRESS_SHORT);
}
size_t fb_screen_text(char *out, size_t cap) { return ui_visible_text(out, cap); }
#else
void fb_screen_info(uint16_t *w, uint16_t *h, uint8_t *channels)
{
    *w = *h = 0;
    *channels = 0;
}
void fb_panel(uint8_t *px) { (void)px; }
void fb_frame(uint8_t *frame) { (void)frame; }
void fb_touch(int pressed, uint16_t x, uint16_t y)
{
    (void)pressed;
    (void)x;
    (void)y;
}
void fb_display_state(fb_display_t *out) { memset(out, 0, sizeof(*out)); out->powered = 1; }
void fb_button(uint8_t btn, int press)
{
    (void)btn;
    (void)press;
}
void fb_set_ghosting(int on) { (void)on; }
void fb_set_refresh_timing(uint32_t partial_ms, uint32_t full_ms)
{
    (void)partial_ms;
    (void)full_ms;
}
size_t fb_screen_text(char *out, size_t cap)
{
    if (cap) {
        out[0] = 0;
    }
    return 0;
}
#endif
