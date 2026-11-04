/*
 * UI glue shared by the variants (PLAN D51): LVGL on a board_api display with two backends and two
 * layouts chosen from the display. 1-bit e-paper (Paper): whole frames with the refresh policy (full
 * refresh every display.full_refresh_every partials (a clock redraw counts one, anything else a quarter, and
 * one that falls due while the board is in use waits: D83), never refresh an unchanged frame, panel asleep
 * between refreshes), a PLACEHOLDER card screen and the idle clock/climate screen; the clock redraws once
 * a minute, one partial refresh. RGB565 LCD (LCD): only changed areas are drawn and sent (D53), the
 * light colour layout, touch and the backlight policy (dim, then off, after idle time; D55), the weather
 * row on the idle screen, the pairing screen and the Wi-Fi setup screen (D60-D64).
 *
 * Single-threaded: call everything from the UI task. The firmware glue queues NotificationEvents
 * from the core task (PLAN section 3). LVGL state is global, so there is one UI per process.
 * Portable: LVGL and board_api only, no ESP-IDF headers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/blob_storage.h"
#include "epager/board_api.h"
#include "epager/npipe.h"
#include "epager/wx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_MAX_CARDS 8u          /* newest shown notifications kept by the placeholder UI */
#define UI_TEXT_MAX 2048u        /* ui_visible_text() */
#define UI_CARDS_TIMEOUT_S 60u   /* ui_params_t.cards_timeout_s: a card goes back to the clock after this long idle */
#define UI_CARD_MIN_MS 2000u     /* a card on the panel stays at least this long before a newer notification replaces it */

typedef enum {
    UI_ACTION_NONE = 0,
    UI_ACTION_PAIRING_TOGGLE,    /* BOOT + PWR held (paper); both volume keys held (boards without BOOT); the settings page's
                                  * button; swipe down on the pairing screen */
    UI_ACTION_POWER_TOGGLE,      /* PWR held 5 s (paper, BOARD_PRESS_HOLD; PLAN D96) */
    UI_ACTION_DISMISS,           /* the user dismissed card `arg` (a notification id): core_dismiss */
    UI_ACTION_BRIGHTNESS_UP,     /* long key press: the firmware changes display.brightness */
    UI_ACTION_BRIGHTNESS_DOWN,
    UI_ACTION_WIFI_SETUP,        /* "Wi-Fi setup" on the pairing screen (short BOOT on the Paper): open the setup hotspot */
    UI_ACTION_WIFI_SETUP_CLOSE,  /* "Close" on the setup screen, or a key: close the hotspot */
    UI_ACTION_SETTING,           /* the Paper's settings list changed a value: arg = (config key << 8) | value, sent
                                  * through on_action; the firmware stores it like a CONFIG_SET (PLAN D96) */
    UI_ACTION_INFO_REFRESH,      /* a settings page that shows ui_info_t opened or changed: the firmware answers with
                                  * ui_set_info (through on_action, PLAN D97) */
    UI_ACTION_FORGET_PHONE,      /* arg = the index of the row in ui_info_t.phone_*: forget that bonded phone */
    UI_ACTION_FORGET_NETWORK,    /* arg = the index in ui_info_t.networks: forget that saved Wi-Fi network */
    /* Firmware updates over Wi-Fi (the Software update page and its prompt; docs/wifi-ota-plan.md) */
    UI_ACTION_UPDATE_CHECK,      /* "Check now" */
    UI_ACTION_UPDATE_INSTALL,    /* "Install" */
    UI_ACTION_UPDATE_LATER,      /* "Remind later" */
    UI_ACTION_UPDATE_SKIP,       /* "Skip this version" */
    UI_ACTION_UPDATE_CANCEL,     /* "Cancel" while the image downloads */
} ui_action_t;

/* The firmware update, as the firmware reports it (ui_set_update). */
typedef enum {
    UI_UPD_NONE = 0,             /* nothing checked yet */
    UI_UPD_CHECKING,
    UI_UPD_UP_TO_DATE,
    UI_UPD_AVAILABLE,            /* `version` is newer; `skipped` says the person skipped it */
    UI_UPD_DOWNLOADING,          /* the percentage is ui_status_t.ota_pct */
    UI_UPD_FAILED,               /* `error` says why */
    UI_UPD_NEEDS_APP,            /* the newest release needs a newer companion app */
} ui_update_state_t;

typedef enum {
    UI_UPD_ERR_NONE = 0,
    UI_UPD_ERR_NO_NETWORK,       /* no Wi-Fi network saved */
    UI_UPD_ERR_NET,              /* could not join, or the release host did not answer */
    UI_UPD_ERR_CLOCK,
    UI_UPD_ERR_DOWNLOAD,
    UI_UPD_ERR_REJECTED,         /* the image check failed */
    UI_UPD_ERR_BUSY,             /* an update from the phone is running */
    UI_UPD_ERR_CANCELLED,
    UI_UPD_ERR_BATTERY,          /* below the update battery limit, not charging */
    UI_UPD_ERR_INTERNAL,
    UI_UPD_ERR_COUNT
} ui_update_error_t;

typedef struct {
    ui_update_state_t state;
    ui_update_error_t error;     /* FAILED */
    char version[33];            /* the newest release found ("" = none) */
    uint32_t size;               /* its image, bytes (0 = unknown) */
    bool skipped;                /* AVAILABLE: the person skipped this version */
    bool mark;                   /* the clock screen says an update is available (AVAILABLE and not skipped) */
} ui_update_t;

/* The Wi-Fi setup hotspot (PLAN D61), as the firmware reports it. */
typedef enum {
    UI_SETUP_OFF = 0,
    UI_SETUP_WAITING,            /* hotspot up: QR code, its name and password, then the page address */
    UI_SETUP_JOINING,            /* the page sent a network: trying it */
    UI_SETUP_DONE,               /* joined it */
    UI_SETUP_FAILED,             /* could not join it (the page may send another one) */
} ui_setup_state_t;

typedef struct {
    ui_setup_state_t state;
    char ap_ssid[33];            /* the hotspot */
    char ap_password[17];
    char url[32];                /* the setup page as shown, e.g. "192.168.4.1" */
    char network[33];            /* JOINING / DONE / FAILED: the network the page sent */
} ui_wifi_setup_t;

/* A short message over the bottom line (Paper), e.g. when the phone changed a setting (PLAN D79). */
typedef enum {
    UI_NOTICE_NONE = 0,
    UI_NOTICE_SETTING,           /* "Setting updated": a CONFIG_SET from the phone was applied */
    UI_NOTICE_WIFI,              /* "Wi-Fi saved": WIFI_SET stored a network */
} ui_notice_t;
#define UI_NOTICE_MS 3000u       /* how long it stays */

/* Which icon the shown card uses (tests and tools). */
typedef enum {
    UI_ICON_NONE = 0,            /* no card shown */
    UI_ICON_BUILTIN,             /* the built-in icon of the notification's category */
    UI_ICON_APP,                 /* an installed app_icon asset (protocol.md 6.4) */
} ui_icon_t;

/* Which connection icon the status bar shows (tests and tools). */
typedef enum {
    UI_LINK_NONE = 0,            /* none (powered off) */
    UI_LINK_CONNECTED,           /* Bluetooth rune: a phone is connected */
    UI_LINK_DISCONNECTED,        /* the rune struck through */
} ui_link_icon_t;

/* Which Wi-Fi icon the status bar shows (tests and tools). The radio is on only to fetch (PLAN D60): the icon is
 * shown while it is connected and hidden otherwise (user, 2026-10-15). CONNECTING and FAILED are not used now. */
typedef enum {
    UI_WIFI_NONE = 0,            /* radio off or not connected, no network stored, or powered off */
    UI_WIFI_OK,                  /* the fan: connected */
    UI_WIFI_CONNECTING,          /* the fan, muted (unused) */
    UI_WIFI_FAILED,              /* the fan struck through (unused) */
} ui_wifi_icon_t;

typedef struct {
    const board_display_t *display; /* 1-bit with width a multiple of 8 (frame path), or RGB565 (area path) */
    const blob_storage_t *blob;     /* installed assets, read for app icons; NULL: built-in icons only */
    bool climate;                   /* the board has a temperature/humidity sensor (idle screen line) */
    uint32_t (*now_ms)(void *ctx);  /* LVGL tick */
    void *ctx;
    /* NULL: no touch panel. Polled by LVGL's input device from ui_service (board_profile_t.read_touch). */
    bool (*read_touch)(void *ctx, board_touch_t *out);
    void *touch_ctx;
    /* Actions that come from touch (keys return theirs from ui_on_button). Called from ui_service. */
    void (*on_action)(void *ctx, ui_action_t action, uint32_t arg);
    /* Seconds without any touch, key or new notification after which the cards give way to the clock screen
     * (the notifications stay, the badge counts them). 0 = a card stays until it is dismissed. */
    uint16_t cards_timeout_s;
    bool wifi_setup;                /* the pairing screen offers "Wi-Fi setup" (boards with Wi-Fi: a button, or short BOOT) */
    bool update;                    /* the settings list has the firmware update rows (boards with Wi-Fi updates) */
} ui_params_t;

typedef struct {
    uint8_t language;            /* PROTO_LANGUAGE_* */
    bool show_body;
    bool mask_numbers;           /* privacy: numbers of 4+ digits are drawn as **** (ui_mask_numbers) */
    uint8_t full_refresh_every;  /* 1..100 */
    int16_t tz_offset_min;
    bool clock_24h;              /* false: 12-hour with AM/PM */
    uint8_t temp_unit;           /* PROTO_TEMP_UNIT_* */
    /* backlight (displays with set_backlight; protocol.md 5): percent while active, then dimmed after
     * idle_dim_s and off after idle_off_s without activity (0 = never) */
    uint8_t brightness;
    uint16_t idle_dim_s, idle_off_s;
    uint8_t orientation;         /* display.orientation (quarter turns), for displays with set_rotation */
    bool weather_enabled;        /* weather.enabled (the Paper's settings list shows it) */
    bool update_auto_check;      /* update.auto_check */
    /* The clock face on the idle screen: ui.clock_face_id ("" = Classic) and ui.clock_face_opts (id, value) pairs. An
     * id that is not installed, a face made for another screen size and a damaged file all show Classic. */
    char clock_face_id[33];
    uint8_t clock_face_opts[32];
    uint8_t clock_face_opts_len;
} ui_config_t;

/* What the Paper's settings pages list (PLAN D97): the bonded phones, the saved Wi-Fi networks, the About page. */
#define UI_PHONES_MAX 4u
#define UI_NETWORKS_MAX 3u
typedef struct {
    uint8_t n_phones;
    bool phone_connected[UI_PHONES_MAX];
    uint8_t n_networks;
    char networks[UI_NETWORKS_MAX][33];
    char firmware[32], device[24], board[28]; /* About: firmware version, the BLE name, the board id */
} ui_info_t;

typedef struct {
    uint8_t battery_pct;         /* 255 = unknown */
    uint8_t wifi_state;          /* PROTO_WIFI_STATE_* (DEVICE_STATUS); 0 = unconfigured, also boards without Wi-Fi */
    bool external_power, connected, pairing_mode;
    bool has_passkey;            /* pairing is in progress: show the code the phone asks for (DisplayOnly, PLAN D6) */
    uint32_t passkey;            /* 0-999999, shown with leading zeros */
    int16_t ota_pct;             /* -1 = no update running */
} ui_status_t;

typedef struct {
    uint32_t partial, full;      /* refreshes started */
    uint32_t partials_since_full;
    uint32_t skipped;            /* rendered frames identical to the panel, not refreshed */
    uint8_t last_kind;           /* board_refresh_t of the last refresh */
} ui_stats_t;

bool ui_init(const ui_params_t *p);
void ui_deinit(void);
void ui_set_config(const ui_config_t *c);
/* Writes src to dst with every number of 4 or more digits replaced by "****": a number is a run of ASCII
 * digits, and a single '.' or ',' between two digits keeps the run going (1.234 and 1,234,567 are one
 * number each). "123" stays. dst may be src (the result is never longer); truncated to cap-1 bytes. */
void ui_mask_numbers(char *dst, size_t cap, const char *src);
void ui_set_status(const ui_status_t *s);
/* Current UTC time, 0 = unknown (shown as --:--). Call every second or so: only a change of the shown
 * minute redraws. */
void ui_set_time(uint32_t utc_s);
/* Latest sensor reading; NULL = the measurement failed (shown as dashes). */
void ui_set_climate(const board_climate_t *c);
/* Weather for the idle screen; NULL = none (the idle screen looks as without Wi-Fi). Only
 * a change of what is shown redraws. */
void ui_set_weather(const wx_reading_t *r);
/* The lists and strings of the settings pages; call when asked (UI_ACTION_INFO_REFRESH) and whenever they change. */
void ui_set_info(const ui_info_t *info);
/* The firmware update state for the Software update page and the clock screen's mark. Cheap to call often: only
 * a change redraws. */
void ui_set_update(const ui_update_t *u);
/* Open the Software update page as a prompt (Install, Remind later, Skip this version) if the clock screen is
 * showing and the screen is on; false otherwise (the firmware tries again later). Backing out, or leaving it
 * alone until the settings time out, is "remind later". */
bool ui_show_update_prompt(void);
/* The setup hotspot's state; its screen is shown while state != UI_SETUP_OFF. */
void ui_set_wifi_setup(const ui_wifi_setup_t *s);
/* Show `n` for UI_NOTICE_MS (a newer one replaces it). Layouts without room for it ignore it (LCD). */
void ui_show_notice(ui_notice_t n);
/* The message on screen now, UI_NOTICE_NONE after it expired (tests and tools). */
ui_notice_t ui_current_notice(void);
/* A shown notification was added, modified or removed (npipe on_event). */
void ui_on_notification(const npipe_notif_t *n);
/* An asset was installed or deleted (core on_asset): app icons are looked up again at the next render. */
void ui_assets_changed(uint8_t asset_type);
/* The id of the clock face the idle screen draws now, "" when it is Classic (tests and tools). */
const char *ui_clock_face_shown(void);
/* How many times a selected face could not be shown (missing, wrong size, damaged, too big for memory). */
uint32_t ui_clock_face_errors(void);
ui_icon_t ui_current_icon(void);
ui_link_icon_t ui_link_icon(void);
ui_wifi_icon_t ui_wifi_icon(void);
/* LCD boards: both keys together (BOARD_BTN_UP_DOWN, SHORT) switch the screen off at once; any key wakes it. */
ui_action_t ui_on_button(board_button_t btn, board_press_t press);
void ui_set_power(bool on);      /* off: a "powered off" screen, buttons except PWR long ignored */
/* Something happened that should wake the screen (the UI also counts touches, keys and new shown
 * notifications by itself). */
void ui_activity(void);
/* Backlight level the UI set last (percent; 0 = off or no backlight). */
uint8_t ui_backlight(void);
/* The screen is off (display.idle_off_s expired, or both keys were pressed together): the firmware may let
 * the chip light-sleep (PLAN D17) until a key, a touch or a notification wakes the screen. False on
 * displays without a backlight. */
bool ui_screen_off(void);
/* display.brightness after a long key press: 10 % steps within 10..100 (the keys never switch the
 * backlight off; the app can set 0). */
uint8_t ui_brightness_step(uint8_t brightness, bool up);
/* Render pending changes and push the frame when the display is free. Returns true while work is
 * pending (a frame waiting for the display, or a refresh in progress). Call every ~20 ms. */
bool ui_service(void);
void ui_get_stats(ui_stats_t *out);
/* Text of the visible labels, one per line (NUL-terminated), for tools and tests. */
size_t ui_visible_text(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
