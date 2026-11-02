/*
 * Board abstraction (PLAN section 3): what shared code may know about a board. Each board in
 * boards/<board> implements board_get(); variants pick the board at build time. Pins and
 * controller details stay private to the board. Plain C, no ESP-IDF or LVGL types, so a host-only
 * simulated board on the PC implements the same interface.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- display */

typedef enum {
    BOARD_REFRESH_PARTIAL = 0, /* e-paper: fast update, may leave ghosting */
    BOARD_REFRESH_FULL = 1,    /* e-paper: slow update with the black/white flash, clears ghosting */
} board_refresh_t;

typedef struct {
    uint16_t width, height;    /* pixels in the panel's native orientation */
    uint8_t display_type;      /* PROTO_DISPLAY_TYPE_* */
    uint8_t color_depth;       /* bits per pixel */
    uint8_t refresh_class;     /* PROTO_REFRESH_CLASS_* */
    /* Pixels the layout keeps what sits in the top corners (the status bar) clear of the left and right screen
     * edges, on top of its own margin: rounded panel corners. 0 = square corners. */
    uint8_t edge_inset;
} board_display_info_t;

/*
 * Two ways to draw; a display implements one of them (PLAN D29, D46):
 * - frame path (e-paper): write_frame + refresh/busy. Frame format for 1-bit displays: rows top to
 *   bottom, ceil(width / 8) bytes per row, most significant bit = leftmost pixel, 1 = white (the same
 *   as the mono1 asset format; the driver converts if its controller differs).
 * - area path (LCD, colour_depth 16): write_area. Pixels are RGB565 in CPU byte order (little-endian,
 *   the rgb565 asset format), w * h of them, rows top to bottom; the driver converts to the wire order.
 *   write_frame, refresh and busy are NULL.
 * Coordinates are in the orientation get_info reports (width x height).
 */
typedef struct {
    void (*get_info)(void *ctx, board_display_info_t *out);
    bool (*init)(void *ctx);                  /* power the panel and reset the controller */
    /* Load a whole frame into the controller's memory. Only while not busy. */
    void (*write_frame)(void *ctx, const uint8_t *frame);
    /* Show the loaded frame. Asynchronous on e-paper: poll busy() until it ends. */
    void (*refresh)(void *ctx, board_refresh_t kind);
    bool (*busy)(void *ctx);
    /* Refresh policy hook: partials since the last full refresh, and the configured
     * display.full_refresh_every. LCD drivers always answer FULL (a plain redraw). */
    board_refresh_t (*choose_refresh)(void *ctx, uint32_t partials_since_full, uint32_t full_every);
    void (*sleep)(void *ctx);                 /* panel and its rail off between refreshes */
    void (*wake)(void *ctx);
    /* LCD only (NULL otherwise). Copies the pixels: the caller's buffer is free again when the call
     * returns; the driver overlaps its transfers with the copy. Areas outside the panel are clipped. */
    void (*write_area)(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *px);
    /* NULL: no adjustable backlight. 0 = off, 100 = full. */
    void (*set_backlight)(void *ctx, uint8_t pct);
    /* NULL: fixed orientation. Quarter turns (display.orientation); a board that supports fewer shows
     * the nearest one it can. The board turns the panel and the touch together: get_info and display
     * coordinates do not change (PLAN D58). The caller redraws the whole screen afterwards. */
    void (*set_rotation)(void *ctx, uint8_t quarter_turns);
    void *ctx;
} board_display_t;

/* ---------------------------------------------------------------- buttons and power */

typedef enum {
    BOARD_BTN_BOOT = 0,
    BOARD_BTN_PWR = 1,
    BOARD_BTN_UP = 2,          /* e.g. volume up */
    BOARD_BTN_DOWN = 3,        /* e.g. volume down */
    BOARD_BTN_UP_DOWN = 4,     /* both keys: held 3 s = LONG (pairing mode on boards without BOOT), pressed and
                              * released together = SHORT (sleep now) */
    BOARD_BTN_BOOT_PWR = 5,    /* BOOT and PWR together (paper): held = LONG (pairing mode); never SHORT */
} board_button_t;
/* HOLD (PWR on the paper, 5 s, PLAN D96) comes after LONG while the key stays down: LONG at 1.5 s, HOLD at 5 s. */
typedef enum { BOARD_PRESS_SHORT = 0, BOARD_PRESS_LONG = 1, BOARD_PRESS_HOLD = 2 } board_press_t;
typedef void (*board_button_cb)(void *user, board_button_t btn, board_press_t press);

/* Mono 16-bit sample number `index` of a sound the board plays (board_util.h has the boot chime). */
typedef int16_t (*board_audio_fn)(void *user, uint32_t index);

typedef enum {
    BOARD_RAIL_DISPLAY = 0,    /* e-paper supply */
    BOARD_RAIL_AUDIO = 1,
    BOARD_RAIL_LATCH = 2,      /* battery power latch: keep on while running */
} board_rail_t;

/* ---------------------------------------------------------------- touch */

typedef struct {
    bool pressed;              /* a finger is on the panel; x, y are valid only then */
    uint16_t x, y;             /* display coordinates (the orientation board_display_info_t reports) */
} board_touch_t;

/* ---------------------------------------------------------------- sensors */

typedef struct {
    int16_t temp_centi_c;      /* hundredths of a degree Celsius */
    uint16_t rh_centi_pct;     /* hundredths of a percent relative humidity, 0..10000 */
} board_climate_t;

/* ---------------------------------------------------------------- battery details */

/* The battery reader's last reading in the board's own terms, for the battery log (components/app,
 * CONFIG_EPAGER_BATTERY_LOG): what the level came from, to check the board's level table. */
typedef struct {
    int32_t raw;               /* this reading in the board's unit (ostb: ADC counts, averaged) */
    int32_t smoothed;          /* what the level was taken from, after the board's smoothing (= raw if none) */
    uint8_t pct;               /* the level read_battery returned */
    bool charging;             /* the charger reports charging (not just "on the cable") */
} board_battery_diag_t;

/* ---------------------------------------------------------------- profile */

typedef struct {
    const char *board_id;      /* CAPABILITIES.board_id, e.g. "waveshare_s3_epaper_154_v2" */
    uint32_t flash_size;       /* bytes */
    uint32_t features;         /* PROTO_FEATURES_* the hardware supports */
    uint8_t low_battery_ota_pct; /* OTA refused below this charge without external power */
    const board_display_t *display;
    /* NULL: no battery reader. False if the level cannot be read. external: PROTO_EXTERNAL_POWER_*. */
    bool (*read_battery)(void *ctx, uint8_t *pct, uint8_t *external);
    /* NULL: no details. The last read_battery reading (it does not read again); false before the first. */
    bool (*battery_diag)(void *ctx, board_battery_diag_t *out);
    /* NULL: no buttons. Short/long presses are reported from the board's button task or timer. */
    void (*set_button_handler)(void *ctx, board_button_cb cb, void *user);
    void (*set_rail)(void *ctx, board_rail_t rail, bool on); /* NULL: no switchable rails */
    /* NULL: nothing to arm. The app is idle (sleeping = true) or busy again (false), PLAN D17: stop the key
     * polling timer and arm the keys as light-sleep wake-up sources; wake_isr(user) runs in interrupt
     * context when one is pressed (the polling then restarts: the app calls this again with false).
     * Returns false when it could not arm (a key is down: the press in progress must be timed by the
     * polling); the app asks again later. Disarming always succeeds. */
    bool (*set_sleep)(void *ctx, bool sleeping, void (*wake_isr)(void *user), void *user);
    /* NULL: no RTC. Real-time clock in UTC seconds (0 = not set). */
    bool (*rtc_get)(void *ctx, uint32_t *utc_s);
    bool (*rtc_set)(void *ctx, uint32_t utc_s);
    /* NULL: no temperature/humidity sensor. False if the measurement failed. Blocks for one
     * measurement (milliseconds): call it from the task that owns the board, about once a minute. */
    bool (*read_climate)(void *ctx, board_climate_t *out);
    /* NULL: no touch panel. Polled by the UI (tens of ms); false if the controller did not answer. */
    bool (*read_touch)(void *ctx, board_touch_t *out);
    /* NULL: the board cannot switch itself off. Called after the "Powered off" screen is on the panel (PLAN D88):
     * release the power latch, wait for the key to be let go and, if the chip is still powered (USB), deep-sleep
     * until the key is pressed again. Does not return on a board that has one. */
    void (*power_off)(void *ctx);
    /* NULL: no speaker. Play n_samples mono samples at rate_hz (PLAN D89); blocks until they have been played,
     * from the task that owns the board (it uses the I2C bus). False if the audio chain did not come up. */
    bool (*play_audio)(void *ctx, board_audio_fn sample, void *user, uint32_t n_samples, uint32_t rate_hz);
    void *ctx;
    /* NULL: nothing to do. Called first thing at boot, before any other board call: take the power
     * latch and put rails and pins in a safe state. Display init stays in display->init. */
    bool (*init)(void *ctx);
} board_profile_t;

/* The board this image was built for. */
const board_profile_t *board_get(void);

#ifdef __cplusplus
}
#endif
