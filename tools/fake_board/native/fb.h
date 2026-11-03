/*
 * Fake board shim: a flat C API over the board core (and, in libepager_sim.so / libepager_sim_lcd.so,
 * the simulated Paper or LCD board and the UI) for tools/fake_board, loaded with ctypes. One
 * board per process.
 * Development tooling only: never part of firmware.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "epager/npipe.h"

#define FB_API __attribute__((visibility("default")))

#define FB_CHR_BATTERY 6u /* Battery Level (TCP char_id, protocol.md 10) */

typedef enum {
    FB_EV_NOTIF = 1,      /* a = kind (npipe_kind_t), plus the notification fields */
    FB_EV_CONFIG = 2,     /* a = number of changed keys, b = config revision */
    FB_EV_OTA = 3,        /* a = xfer_ota_event_kind_t, b = pct, c = state */
    FB_EV_DISCONNECT = 4, /* the board dropped the link (reboot, power off) */
    FB_EV_REBOOT = 5,     /* a = image pending verify, text = running version */
    FB_EV_ACTION = 6,     /* a = ui_action_t from a button or touch, b = its argument */
    FB_EV_POWER = 7,      /* a = 1 on, 0 off */
} fb_event_type_t;

typedef struct {
    uint32_t type, a, b, c;
    /* FB_EV_NOTIF: the display-ready NotificationEvent */
    uint32_t id, timestamp;
    uint8_t source, category, priority, flags, shown, pad[3];
    char app_id[NPIPE_APP_ID_MAX + 1];
    char app_name[NPIPE_APP_NAME_MAX + 1];
    char title[NPIPE_TITLE_MAX + 1];
    char subtitle[NPIPE_SUBTITLE_MAX + 1];
    char body[NPIPE_BODY_MAX + 1];
    char text[64];
} fb_event_t;

typedef struct {
    uint32_t refreshing, asleep, kind, phase, elapsed_ms;
    uint32_t partial, full, violations;          /* completed by the panel */
    uint32_t ui_partial, ui_full, ui_skipped, partials_since_full; /* started by the UI */
    uint32_t powered;                            /* board power (PWR long press) */
    uint32_t link;                               /* status-bar connection icon: ui_link_icon_t */
    uint32_t backlight;                          /* LCD: percent set last (e-paper: 0) */
    uint32_t wifi;                               /* status-bar Wi-Fi icon: ui_wifi_icon_t */
} fb_display_t;

FB_API size_t fb_event_size(void);
FB_API size_t fb_display_size(void);
FB_API int fb_has_screen(void);

/* Fresh board: empty storage of the given size, firmware slot, running version. */
FB_API int fb_init(uint32_t storage_total, uint32_t fw_slot, const char *version, int allow_downgrade,
                   const char *device_name);
/* Simulated reset or power cut: RAM state lost (nothing journaled at that moment), storage kept,
 * link dropped. */
FB_API void fb_reboot(void);
FB_API void fb_set_time(uint32_t now_ms);
FB_API void fb_tick(void);

FB_API void fb_connect(uint16_t mtu, int secure);
FB_API void fb_disconnect(void);
FB_API void fb_set_secure(int secure);
FB_API int fb_connected(void);
FB_API void fb_write(uint8_t chr, const uint8_t *data, size_t len);
FB_API size_t fb_read(uint8_t chr, uint8_t *out, size_t cap);
/* Next queued notification: returns its length and sets *chr, or -1 if none. */
FB_API int fb_poll(uint8_t *chr, uint8_t *out, size_t cap);
FB_API int fb_poll_event(fb_event_t *out);

/* Simulation controls */
FB_API void fb_set_battery(int reader, int readable, uint8_t pct, uint8_t external_power);
FB_API void fb_set_utc(uint32_t utc_s); /* the soft clock only (TIME_SYNC also writes the RTC) */
/* Simulated board only (no-ops otherwise). RTC: present = 0 removes it, utc_s = 0 = no valid time;
 * presence applies at the next boot. Sensor: present = 0 removes it (next boot), readable = 0 fails
 * the readings; the new reading is shown at once (the firmware reads it once a minute). */
FB_API void fb_set_rtc(int present, uint32_t utc_s);
FB_API uint32_t fb_rtc_utc(void);
FB_API void fb_set_climate(int present, int readable, int16_t centi_c, uint16_t centi_rh);
FB_API void fb_set_pairing(int on);
FB_API void fb_set_bonded(uint8_t n);
FB_API void fb_set_storage_total(uint32_t total);
FB_API void fb_storage_budget(int32_t writes); /* < 0: unlimited; else storage fails after n more writes */
FB_API int fb_corrupt_transfer(void);          /* flip a byte of the incomplete transfer's data */
FB_API void fb_set_fw_image_status(uint8_t status);
FB_API void fb_set_self_check_fail(int fail);
FB_API void fb_ancs(uint8_t event_id, uint8_t event_flags, uint8_t category, uint32_t uid, const char *app_id,
                    const char *app_name, const char *title, const char *subtitle, const char *message,
                    const char *date);
FB_API void fb_ancs_reset(void);
/* Android notification as the app would post it (bypasses the link; for the CLI). Returns the
 * protocol status; *id = board id. */
FB_API int fb_android(const char *key, const char *app_id, const char *app_name, const char *title,
                      const char *subtitle, const char *body, uint8_t category, uint8_t priority, uint8_t flags,
                      uint32_t *id);
FB_API int fb_android_remove(const char *key);
/* Wi-Fi (boards with the wifi feature): the network WIFI_SET stored, "" = none; returns the name's length. */
FB_API size_t fb_wifi_network(char *ssid, size_t cap, char *password, size_t pw_cap);
FB_API void fb_set_wifi_state(uint8_t state);  /* DEVICE_STATUS wifi_state (the stored name is kept) */
FB_API void fb_set_weather_state(uint8_t state);
/* Weather on the idle screen (simulator libraries; no-ops otherwise): present = 0 removes it; place
 * (may be NULL or "") is the name shown under it. */
FB_API void fb_weather(int present, int16_t temp_c10, int32_t wmo_code, int is_day, int16_t high_c10,
                       int16_t low_c10, uint8_t precip_pct, const char *place);
/* An Open-Meteo forecast document through the board's parser, shown with `place` if it parses; returns
 * wx_result_t (simulator libraries; -1 otherwise). */
FB_API int fb_weather_json(const char *json, size_t len, const char *place);
/* The firmware update as the UI shows it (ui_update_state_t, ui_update_error_t; simulator libraries): the Software
 * update page and the clock screen's mark. fb_update_prompt opens the prompt (1 if it opened). */
FB_API void fb_set_update(uint8_t state, uint8_t error, const char *version, uint32_t size, int skipped);
FB_API int fb_update_prompt(void);
/* The Wi-Fi setup screen (ui_setup_state_t; simulator libraries). */
FB_API void fb_wifi_setup(uint8_t state, const char *ap_ssid, const char *ap_password, const char *url,
                          const char *network);

/* State */
FB_API int32_t fb_config_int(uint16_t key);
FB_API uint32_t fb_config_revision(void);
FB_API uint32_t fb_notif_count(int shown_only);
FB_API uint8_t fb_ota_state(void);
FB_API uint8_t fb_battery_pct(void);
FB_API uint32_t fb_now_utc(void);
FB_API size_t fb_version(char *out, size_t cap);

/* Screen (simulator libraries only; no-ops returning 0 otherwise) */
/* Panel size and bytes per pixel of fb_panel: 1 (grey, e-paper) or 3 (RGB, LCD). */
FB_API void fb_screen_info(uint16_t *w, uint16_t *h, uint8_t *channels);
FB_API void fb_panel(uint8_t *px);             /* what a person sees: width * height * channels bytes */
FB_API void fb_frame(uint8_t *frame);          /* e-paper: last completed 1-bit frame; LCD: RGB565 RAM */
FB_API void fb_touch(int pressed, uint16_t x, uint16_t y); /* LCD only */
FB_API void fb_display_state(fb_display_t *out);
FB_API void fb_button(uint8_t btn, int press); /* press: 0 short, 1 long, 2 hold */
FB_API void fb_set_ghosting(int on);
FB_API void fb_set_refresh_timing(uint32_t partial_ms, uint32_t full_ms);
FB_API size_t fb_screen_text(char *out, size_t cap);
