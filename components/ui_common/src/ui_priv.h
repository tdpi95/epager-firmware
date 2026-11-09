/*
 * Inside ui_common (PLAN D51): the shared model and LVGL glue (ui.c), display backends that own the LVGL
 * display and move pixels to the board, and layouts that own the widgets and draw the model. The layout
 * and the backend follow the board's display (colour depth). Single-threaded, like the public API.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/board_util.h"
#include "epager/ui.h"
#include "lvgl.h"

/* Firmware compiles in only the layouts its variant enables (components/ui_common/Kconfig); the host
 * build has both. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_EPAGER_UI_MONO
#define UI_HAS_MONO 1
#else
#define UI_HAS_MONO 0
#endif
#ifdef CONFIG_EPAGER_UI_COLOUR
#define UI_HAS_COLOUR 1
#else
#define UI_HAS_COLOUR 0
#endif
#else
#define UI_HAS_MONO 1
#define UI_HAS_COLOUR 1
#endif

#define TIME_UNKNOWN UINT32_MAX

/* Large static state off the render path: in PSRAM where the image allows it (ESP-IDF's
 * CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY; empty otherwise and on the host). Zero-initialised only. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define UI_EXT_BSS EXT_RAM_BSS_ATTR
#else
#define UI_EXT_BSS
#endif

typedef struct {
    const char *no_time, *pairing, *off, *updating, *temp_cap, *rh_cap;
    const char *wday[7], *month[12];
    bool numeric_date; /* "day/month" instead of "day month-name" */
    /* pairing and Wi-Fi setup screens (colour layout) */
    const char *pair_hint, *wifi_setup, *close;
    const char *setup_join;    /* printf: hotspot name, password, page address */
    const char *setup_joining, *setup_done, *setup_failed; /* printf: the network */
    const char *settings;      /* settings page title */
    const char *pair_code;     /* above the passkey (colour layout) */
    const char *pair_code_short; /* the Paper's title line, which does not wrap */
    /* the Paper's full-screen pairing and setup screens (PLAN D78) */
    const char *pair_key_wifi;   /* "BOOT: Wi-Fi setup" */
    const char *pair_key_cancel; /* "Hold BOOT+PWR: cancel" */
    const char *setup_join_short; /* printf: hotspot name, password, page address; three lines */
    const char *setup_key_close; /* "Any key: close" */
    const char *notice_setting, *notice_wifi; /* the messages of ui_notice_t */
    /* the Paper's settings list (PLAN D96, D97): the rows, the values and the pages */
    const char *set_clock, *set_temp, *set_language, *set_body, *set_mask, *set_rotation, *set_weather;
    const char *set_pairing, *set_wifi, *on, *off_value;
    const char *set_phones, *set_networks, *set_about; /* a row of the list and the title of its page */
    const char *forget_ask, *none_phones, *none_networks, *phone_n, *phone_connected; /* phone_n: printf, the number */
    const char *about_firmware, *about_device, *about_board, *about_battery, *about_protocol;
    /* firmware updates: two rows of the list, the clock screen's mark, the page's states and actions */
    const char *set_autocheck, *set_update, *upd_mark;
    const char *upd_none, *upd_checking, *upd_current, *upd_available, *upd_skipped, *upd_updating, *upd_needs_app, *upd_size;
    const char *upd_install, *upd_check, *upd_later, *upd_skip, *upd_cancel;
    const char *upd_err[UI_UPD_ERR_COUNT]; /* [UI_UPD_ERR_NONE] is unused */
    /* clock faces (M8b): the row and page title, the list's marks, the page's rows */
    const char *set_face, *face_classic, *face_active, *face_file, *face_delete, *face_delete_ask;
} ui_strings_t;

/* Owns the LVGL display and its buffers; service pushes what LVGL rendered to the board. */
typedef struct {
    bool (*init)(void);    /* after lv_init: create ui.disp */
    bool (*service)(void); /* true while work is pending (a frame waiting, a refresh in progress) */
    bool (*panel_wait)(void); /* NULL or false: after service, the only work left is the panel's own refresh */
    void (*reset)(void);   /* forget everything (LVGL memory is already freed) */
} ui_backend_t;

/* Owns the widgets: build once, apply the model whenever it changed. */
typedef struct {
    void (*build)(void);
    void (*apply)(void);
    size_t (*visible_text)(char *out, size_t cap);
    void (*reset)(void);
} ui_layout_t;

/* One face of the Clock face page: Classic (id ""), a built-in face, or an installed one. */
#define UI_FACES_MAX 21u /* Classic, up to 4 built-in, 16 installed */
typedef struct {
    char id[33];
    char name[2][33];  /* English, Vietnamese; "" = use the other, or the id */
    bool builtin, local; /* local: installed from a file (origin local) */
} ui_face_entry_t;
/* Fill out[] (at most max): Classic, the built-in faces of this layout, then the installed faces. Returns the count. */
size_t ui_faces_collect(ui_face_entry_t *out, size_t max);
/* The options of the active face for the face page: row i's label and current value text (language of the UI), the option id
 * and the value a select moves to. False past the last option, or when no face is active. */
size_t ui_face_option_count(void);
bool ui_face_option_row(size_t i, const char **label, char *value, size_t cap, uint8_t *id, uint8_t *next);

typedef struct {
    ui_params_t p;
    board_display_info_t info;
    lv_display_t *disp;
    const ui_backend_t *backend;
    const ui_layout_t *layout;
    bool powered, model_dirty;
    bool panel_wait;                   /* ui_panel_wait(): set by ui_service */
    bool settings;                     /* the settings list is open (closed when the screen goes off and after
                                        * cards_timeout_s without a key or a touch) */
    size_t set_cur, set_top;           /* the highlighted (Paper) or last tapped (LCD) row, and the first row on screen */
    uint8_t set_page;                  /* ui_settings_page_t */
    size_t set_main_cur;               /* the row of the main list that opened the page */
    size_t set_faces_cur;              /* the row of the face list that opened a face's page */
    size_t face_sel;                   /* the face of that page: an index into faces[] */
    size_t n_faces;
    ui_face_entry_t faces[UI_FACES_MAX]; /* the face list (ui_faces_collect), read when its page opens */
    bool set_confirm;                  /* the highlighted row asked "Forget?" and waits for a second select */
    ui_info_t lists;                   /* what the settings pages list (ui_set_info) */
    ui_update_t upd;                   /* the firmware update (ui_set_update) */
    bool upd_prompt;                   /* the Software update page was opened by the firmware as a prompt */
    bool show_cards;                   /* the cards are on screen; false = the clock screen with a badge (colour layout: swipe
                                        * up hides them, swipe down brings them back; the Paper always shows them) */
    ui_config_t cfg;
    ui_status_t st;
    ui_stats_t stats;
    npipe_notif_t cards[UI_MAX_CARDS]; /* newest first */
    size_t n_cards, cur;
    uint32_t utc, shown_minute;        /* shown_minute: UTC minute on screen, TIME_UNKNOWN = none */
    bool climate_ok;
    board_climate_t clim;              /* last good reading */
    bool wx_ok;
    wx_reading_t wx;                   /* weather shown on the idle screen */
    ui_wifi_setup_t setup;
    uint32_t shown_id;                 /* the card on the screen (0 = none) and when it reached the panel (D81) */
    uint32_t card_since_ms;
    bool card_since_known;
    /* Notifications that arrived while a card had not been on the panel for UI_CARD_MIN_MS: their ids in arrival
     * order, each opens in turn and stays its minimum time (D81). */
    uint32_t queue[UI_MAX_CARDS];
    size_t n_queue;
    ui_notice_t notice;                /* the message over the bottom line, and when it was shown */
    uint32_t notice_ms;
    ui_icon_t icon_src;
    ui_link_icon_t link_src;
    ui_wifi_icon_t wifi_src;
    /* backlight policy (displays with set_backlight) */
    uint32_t last_activity_ms;
    uint8_t backlight;                 /* level set last */
    bool shown_once, panel_off;        /* a frame is on the panel; panel asleep (backlight off) */
    bool force_off;                    /* "sleep now" (both keys): off as if display.idle_off_s had expired */
    uint32_t assets_gen;               /* bumped when app icons change: layouts load icons again */
} ui_state_t;

extern ui_state_t ui;

extern const ui_backend_t ui_backend_frame; /* e-paper: whole 1-bit frames, refresh policy (D29) */
extern const ui_backend_t ui_backend_area;  /* LCD: changed areas through write_area (D53) */
extern const ui_layout_t ui_layout_mono;    /* the Paper screens */
extern const ui_layout_t ui_layout_colour;  /* the LCD screens */

/* Shared helpers (ui.c) */
const ui_strings_t *ui_strings(void);
/* "14:27", or "2:27" with *ampm = "AM"/"PM" in 12-hour mode; padded selects "09:05" over "9:05". */
void ui_format_time(char *buf, size_t cap, const char **ampm, bool padded);
void ui_format_date(char *buf, size_t cap);
/* A temperature in 0.1 deg C as whole degrees of the configured unit (rounded). */
int32_t ui_temp_shown(int32_t temp_c10);
/* What the climate boxes show, in tenths of the configured unit and whole percent (rounded). */
void ui_climate_shown(const board_climate_t *c, int32_t *temp, int32_t *rh);
/* Hide or show; nothing (no redraw) if the state does not change. */
void ui_set_hidden(lv_obj_t *o, bool hidden);
/* Set a label's text only if it differs (LVGL redraws a label on every set). */
void ui_set_text(lv_obj_t *l, const char *text);
/* A card's title, subtitle or body as drawn: src itself, or with numbers masked into buf when
 * display.mask_numbers is on. buf may be src. */
const char *ui_card_text(char *buf, size_t cap, const char *src);
/* The card on screen, or NULL (powered off or no notification). */
const npipe_notif_t *ui_current_card(void);
/* The idle clock screen is what the panel shows (no card, message, pairing or setup screen). */
bool ui_clock_shown(void);
/* Somebody is using the board: not the clock, or a key or a notification less than UI_FULL_QUIET_MS ago. A full
 * refresh that falls due waits for this to end (PLAN D83). */
bool ui_in_use(void);
#define UI_FULL_QUIET_MS 60000u
uint32_t ui_now_ms(void);
/* Next / previous card (wraps); false with fewer than two cards. */
bool ui_step_card(int dir);
/* DEVICE_STATUS wifi_state to the status bar icon: how the last join went (D60). */
ui_wifi_icon_t ui_wifi_icon_for(uint8_t wifi_state);
/* Weather condition and day/night to ui_weather_icon_t; -1 for an unknown condition (no icon). */
int ui_weather_icon_for(uint8_t cond, bool day);
/* "WIFI:T:WPA;S:<ssid>;P:<password>;;" with \ ; , : " escaped (the common Wi-Fi QR format). */
void ui_wifi_qr_data(char *out, size_t cap, const char *ssid, const char *pw);
/* Report an action from touch to the firmware (ui_params_t.on_action). */
void ui_emit(ui_action_t action, uint32_t arg);
/* Local time (UTC plus the time zone) as shown; false while the time is unknown. */
bool ui_local_time(board_datetime_t *t);

/* The clock face on the idle screen (ui_face.c; contracts/asset-formats.md "Clock faces"). The layouts ask
 * ui_face_sync() first: it (re)loads the face when the selection, the option values, the installed assets or the
 * screen size changed, and falls back to Classic (the layouts' own idle screen) on any failure. */
void ui_face_sync(void);
/* A face, not Classic, is what the idle screen shows. */
bool ui_face_active(void);
/* Draw the face (show) or hide all of it. The face covers the whole screen: the layout hides its own idle objects, its
 * status line and its badge while a face shows. */
void ui_face_apply(bool show);
/* Free the face and its images, before lv_deinit. */
void ui_face_reset(void);
/* The text the face shows now, one line per element (tests and tools: appended to ui_visible_text). */
size_t ui_face_text(char *out, size_t cap);

/* The settings list (ui_settings.c, PLAN D96, D99). Rows that need Wi-Fi are left out on a board without it. */
#define UI_SETTINGS_ROWS 7u /* rows on screen at once on the Paper */
#define UI_TOUCH_ROWS 4u    /* ... on a touch board (the LCD: tall enough to hit with a finger) */
size_t ui_settings_rows_shown(void);
typedef enum {
    UI_PAGE_MAIN = 0, UI_PAGE_PHONES, UI_PAGE_NETWORKS, UI_PAGE_ABOUT, UI_PAGE_UPDATE, UI_PAGE_FACES, UI_PAGE_FACE
} ui_settings_page_t;
size_t ui_settings_count(void);
const char *ui_settings_title(void); /* the header of the list or of the page open */
/* Row i: its label, and the value as shown in `value` (empty for an action). */
void ui_settings_row(size_t i, const char **name, char *value, size_t cap);
void ui_settings_enter(void);       /* open the list at the first row */
/* Long PWR: from a page to the main list (on the row that opened it), from the main list to the clock. */
void ui_settings_back(void);
/* Keep the highlight on a row that exists (the lists shrink when something is forgotten). */
void ui_settings_clamp(void);
void ui_settings_step(int dir);     /* highlight the previous (-1) or next (+1) row, wrapping */
/* Touch: select row `row` (an index into the whole list); an action it returns (pairing, Wi-Fi setup) is reported. */
void ui_settings_tap(size_t row);
/* Touch: the next (+1) or previous (-1) page of rows. */
void ui_settings_scroll(int dir);
/* Select the highlighted row: a choice goes to the next value (UI_ACTION_SETTING through ui_emit), an action
 * returns its ui_action_t. */
ui_action_t ui_settings_select(void);
/* The installed faces changed: the Clock face pages read the list again. */
void ui_settings_faces_changed(void);
void ui_settings_config_changed(void); /* the configuration changed (ui_set_config) */
/* True on a board whose keys drive the UI (the frame path, no touch): the Paper. */
bool ui_keys_only(void);
