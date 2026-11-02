/*
 * Small pure helpers every board can use, header-only and portable (host-tested in
 * tests/host/test_board_api.c) so the logic of a board that cannot run on the PC is still tested.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/board_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- battery */

/*
 * Charge level of a single-cell LiPo from its resting voltage, linear between the points of the
 * usual discharge curve. A stand-in until the Paper's battery is known (PLAN open question:
 * chemistry and capacity); a board with a fuel gauge does not use it.
 */
static inline uint8_t board_lipo_pct(int mv)
{
    static const struct { uint16_t mv; uint8_t pct; } curve[] = {
        {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75}, {3950, 70}, {3910, 65},
        {3870, 60},  {3850, 55}, {3840, 50}, {3820, 45}, {3800, 40}, {3790, 35}, {3770, 30}, {3750, 25},
        {3730, 20},  {3710, 15}, {3690, 10}, {3610, 5},  {3270, 0},
    };
    const size_t n = sizeof(curve) / sizeof(curve[0]);
    if (mv >= curve[0].mv) {
        return 100;
    }
    for (size_t i = 1; i < n; i++) {
        if (mv >= curve[i].mv) {
            int span = curve[i - 1].mv - curve[i].mv;
            return (uint8_t)(curve[i].pct + (curve[i - 1].pct - curve[i].pct) * (mv - curve[i].mv) / span);
        }
    }
    return 0;
}

/*
 * The same curve for a board whose charger ends the charge at `full_mv`, as the board reads it (PLAN D104): the
 * Paper's charger stops with the cell resting at 4090-4114 mV on its ADC, never at the 4200 mV of the table, so
 * its level topped out around 90 %. Above 3750 mV the voltage is stretched so that `full_mv` gives the table's
 * 4200 mV (100 %); the low end, where the shutdown point matters, is unchanged. `full_mv` of 4200 or more, or of
 * 3800 or less, leaves the curve as it is.
 */
#define BOARD_LIPO_STRETCH_FROM_MV 3750
static inline uint8_t board_lipo_pct_full(int mv, int full_mv)
{
    if (full_mv >= 4200 || full_mv <= BOARD_LIPO_STRETCH_FROM_MV + 50 || mv <= BOARD_LIPO_STRETCH_FROM_MV) {
        return board_lipo_pct(mv);
    }
    const int from = BOARD_LIPO_STRETCH_FROM_MV;
    return board_lipo_pct(from + (mv - from) * (4200 - from) / (full_mv - from));
}

/*
 * Steadies the battery level between readings (PLAN D84, D85). A cell's voltage moves with the load (the radio, a
 * panel refresh) and the level curve is steep (100 mV is about 30 %), so a raw reading jumps. The filter keeps the
 * last five readings, takes their median (one outlier does not count), follows it with a slow average, and starts
 * again at once from a step of 150 mV or more. The shown level changes only when it moves by 2 points or more.
 *
 * Charging (no pin senses it): a charger pushes current into the cell, which lifts the terminal voltage by about
 * 60 mV and shows as a step up of 40 mV or more, or as a wide spread of the ADC samples (ripple, 40 mV or more
 * against 10-25 mV at rest). The flag ends with a step down of 40 mV (the charger is unplugged) or after five
 * calm readings (the charge is done). The history starts again whenever the flag changes, and the level is
 * computed from the voltage minus BOARD_BATT_CHARGE_MV while it is set.
 */
#define BOARD_BATT_HIST 5
#define BOARD_BATT_STEP_MV 150
#define BOARD_BATT_HOLD_PCT 2
#define BOARD_BATT_CHARGE_STEP_MV 40
#define BOARD_BATT_CHARGE_SPREAD_MV 40
#define BOARD_BATT_CALM_SPREAD_MV 25
#define BOARD_BATT_CALM_READINGS 5
#define BOARD_BATT_CHARGE_MV 60

typedef struct {
    int16_t hist[BOARD_BATT_HIST];
    uint8_t n, next, calm;
    int32_t avg_mv;
    int16_t shown_pct; /* -1 = none yet */
    bool charging;
} board_batt_filter_t;

static inline void board_batt_filter_init(board_batt_filter_t *f)
{
    *f = (board_batt_filter_t){.shown_pct = -1};
}

static inline int board_batt_median(const board_batt_filter_t *f)
{
    int16_t v[BOARD_BATT_HIST];
    for (uint8_t i = 0; i < f->n; i++) {
        v[i] = f->hist[i];
    }
    for (uint8_t i = 1; i < f->n; i++) { /* insertion sort of at most five */
        int16_t x = v[i];
        uint8_t j = i;
        for (; j > 0 && v[j - 1] > x; j--) {
            v[j] = v[j - 1];
        }
        v[j] = x;
    }
    return v[f->n / 2];
}

/* Feeds one reading (millivolts, and the spread of its samples) and returns the steadied voltage. */
static inline int board_batt_filter_mv(board_batt_filter_t *f, int mv, int spread_mv)
{
    const bool was = f->charging;
    const int delta = f->n ? mv - (int)f->avg_mv : 0;
    if (spread_mv >= BOARD_BATT_CHARGE_SPREAD_MV || delta >= BOARD_BATT_CHARGE_STEP_MV) {
        f->charging = true;
        f->calm = 0;
    } else if (f->charging) {
        if (-delta >= BOARD_BATT_CHARGE_STEP_MV) {
            f->charging = false;
        } else if (spread_mv < BOARD_BATT_CALM_SPREAD_MV) {
            if (++f->calm >= BOARD_BATT_CALM_READINGS) {
                f->charging = false;
            }
        } else {
            f->calm = 0;
        }
    }
    if (f->charging != was || delta >= BOARD_BATT_STEP_MV || -delta >= BOARD_BATT_STEP_MV) {
        f->n = 0; /* a different supply: forget the old readings */
        f->next = 0;
    }
    f->hist[f->next] = (int16_t)mv;
    f->next = (uint8_t)((f->next + 1) % BOARD_BATT_HIST);
    if (f->n < BOARD_BATT_HIST) {
        f->n++;
    }
    const int med = board_batt_median(f);
    f->avg_mv = f->n == 1 ? med : f->avg_mv + (med - f->avg_mv) / 4;
    return (int)f->avg_mv;
}

/* The voltage the level is computed from: the steadied one, less the charger's lift while charging. */
static inline int board_batt_level_mv(const board_batt_filter_t *f, int steadied_mv)
{
    return f->charging ? steadied_mv - BOARD_BATT_CHARGE_MV : steadied_mv;
}

/* The level to show for `pct` computed from the steadied voltage. */
static inline uint8_t board_batt_hold_pct(board_batt_filter_t *f, uint8_t pct)
{
    const int d = pct - f->shown_pct;
    if (f->shown_pct < 0 || d >= BOARD_BATT_HOLD_PCT || d <= -BOARD_BATT_HOLD_PCT || pct == 0 || pct == 100) {
        f->shown_pct = pct;
    }
    return (uint8_t)f->shown_pct;
}

/*
 * Charge level from a reading through a table of ascending readings and their levels, linear
 * between the points and clamped at both ends. For boards whose level table is known in raw ADC
 * counts (no voltage curve yet). n >= 2; xs strictly ascending.
 */
static inline uint8_t board_table_pct(int x, const uint16_t *xs, const uint8_t *pcts, size_t n)
{
    if (x <= xs[0]) {
        return pcts[0];
    }
    for (size_t i = 1; i < n; i++) {
        if (x < xs[i]) {
            int span = xs[i] - xs[i - 1];
            return (uint8_t)(pcts[i - 1] + (pcts[i] - pcts[i - 1]) * (x - xs[i - 1]) / span);
        }
    }
    return pcts[n - 1];
}

/* ---------------------------------------------------------------- buttons */

typedef enum { BOARD_BTN_EV_NONE = 0, BOARD_BTN_EV_SHORT, BOARD_BTN_EV_LONG, BOARD_BTN_EV_HOLD } board_btn_ev_t;

typedef struct {
    uint8_t down_samples;  /* consecutive samples with the key down (saturates) */
    bool down, long_sent, hold_sent; /* debounced press in progress, long and hold already reported */
    uint32_t held_ms;
} board_btn_state_t;

/*
 * Debounced short/long press detector for an active-low key, stepped at a fixed rate (the board's
 * poll timer). A press counts once the key has been down for debounce_samples samples. LONG is
 * reported once, as soon as the press has lasted long_ms (while still held); SHORT is reported on
 * release only if LONG was not. With hold_ms > long_ms a key that stays down also reports HOLD once, when
 * the press has lasted hold_ms (PWR on the paper: power off, PLAN D96); the release then reports nothing.
 * A glitch shorter than the debounce time reports nothing.
 */
static inline board_btn_ev_t board_btn_step_hold(board_btn_state_t *s, bool key_down, uint32_t step_ms,
                                                 uint32_t long_ms, uint32_t hold_ms, uint8_t debounce_samples)
{
    if (key_down) {
        if (s->down_samples < 255) {
            s->down_samples++;
        }
        if (s->down_samples == debounce_samples) {
            s->down = true;
            s->held_ms = 0;
            s->long_sent = s->hold_sent = false;
        } else if (s->down) {
            s->held_ms += step_ms;
            if (!s->long_sent && s->held_ms >= long_ms) {
                s->long_sent = true;
                return BOARD_BTN_EV_LONG;
            }
            if (hold_ms > long_ms && !s->hold_sent && s->held_ms >= hold_ms) {
                s->hold_sent = true;
                return BOARD_BTN_EV_HOLD;
            }
        }
        return BOARD_BTN_EV_NONE;
    }
    board_btn_ev_t ev = s->down && !s->long_sent ? BOARD_BTN_EV_SHORT : BOARD_BTN_EV_NONE;
    s->down = false;
    s->down_samples = 0;
    return ev;
}

static inline board_btn_ev_t board_btn_step(board_btn_state_t *s, bool key_down, uint32_t step_ms,
                                            uint32_t long_ms, uint8_t debounce_samples)
{
    return board_btn_step_hold(s, key_down, step_ms, long_ms, 0, debounce_samples);
}

/*
 * Two keys held together (a chord), for boards without a BOOT key. As soon as both are down the chord is
 * active: the caller stops feeding the single-key detectors (and resets them silently) until both are
 * released, so the chord never also produces short or long presses. Reports once per chord:
 *  - LONG while still held, when both have been held for hold_ms (pairing mode);
 *  - SHORT when both are up again, if the longest time both were down was within [tap_min_ms, tap_max_ms]
 *    (sleep now). Shorter is a key roll-over, longer a chord nobody finished: both report nothing.
 */
typedef struct {
    bool active, sent;
    uint32_t held_ms, peak_ms;
} board_chord_state_t;

static inline board_btn_ev_t board_chord_step(board_chord_state_t *s, bool a_down, bool b_down, uint32_t step_ms,
                                              uint32_t hold_ms, uint32_t tap_min_ms, uint32_t tap_max_ms)
{
    if (!a_down && !b_down) {
        const bool tap = s->active && !s->sent && s->peak_ms >= tap_min_ms && s->peak_ms <= tap_max_ms;
        s->active = s->sent = false; /* over once both are up */
        s->peak_ms = 0;
        return tap ? BOARD_BTN_EV_SHORT : BOARD_BTN_EV_NONE;
    }
    if (!(a_down && b_down)) {
        s->held_ms = 0;
        return BOARD_BTN_EV_NONE; /* one key: a single press, or the chord winding down (still active) */
    }
    if (!s->active) {
        s->active = true;
        s->sent = false;
        s->held_ms = s->peak_ms = 0;
    }
    s->held_ms += step_ms;
    if (s->held_ms > s->peak_ms) {
        s->peak_ms = s->held_ms;
    }
    if (!s->sent && s->held_ms >= hold_ms) {
        s->sent = true;
        return BOARD_BTN_EV_LONG;
    }
    return BOARD_BTN_EV_NONE;
}

/* ---------------------------------------------------------------- calendar */

typedef struct {
    int32_t year;
    uint8_t month, day;        /* 1..12, 1..31 */
    uint8_t hour, min, sec;
    uint8_t wday;              /* 0 = Sunday */
} board_datetime_t;

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil). */
static inline int64_t board_days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153u * (m > 2 ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* Seconds since 1970-01-01 (UTC, or local time when the caller added its offset) to a date. */
static inline void board_datetime_from_s(int64_t s, board_datetime_t *out)
{
    int64_t days = s >= 0 ? s / 86400 : -((-s + 86399) / 86400);
    const int64_t sod = s - days * 86400;
    out->hour = (uint8_t)(sod / 3600);
    out->min = (uint8_t)(sod / 60 % 60);
    out->sec = (uint8_t)(sod % 60);
    out->wday = (uint8_t)(((days + 4) % 7 + 7) % 7); /* 1970-01-01 was a Thursday */
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const uint32_t doe = (uint32_t)(days - era * 146097);
    const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const uint32_t mp = (5u * doy + 2u) / 153u;
    out->day = (uint8_t)(doy - (153u * mp + 2u) / 5u + 1u);
    out->month = (uint8_t)(mp < 10u ? mp + 3u : mp - 9u);
    out->year = (int32_t)((int64_t)yoe + era * 400 + (out->month <= 2));
}

static inline int64_t board_datetime_to_s(const board_datetime_t *t)
{
    return board_days_from_civil(t->year, t->month, t->day) * 86400 + t->hour * 3600 + t->min * 60 + t->sec;
}

static inline uint8_t board_days_in_month(int32_t y, uint8_t m)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : days[(m - 1u) % 12u];
}

/* ---------------------------------------------------------------- RTC: NXP PCF85063 */

/* BCD byte (after masking the field's bits) to binary; false if a digit is above 9 or the value
 * is outside lo..hi. */
static inline bool board_bcd_field(uint8_t v, uint8_t lo, uint8_t hi, uint8_t *out)
{
    if ((v & 0x0Fu) > 9u || (v >> 4) > 9u) {
        return false;
    }
    *out = (uint8_t)((v >> 4) * 10u + (v & 0x0Fu));
    return *out >= lo && *out <= hi;
}

static inline uint8_t board_bcd(uint8_t v) { return (uint8_t)((v / 10u) << 4 | v % 10u); }

#define BOARD_PCF85063_TIME_REGS 7u     /* Seconds .. Years, read and written in one burst */
#define BOARD_PCF85063_OS 0x80u         /* Seconds bit 7: the oscillator stopped, time not valid */
#define BOARD_RTC_MIN_UTC 946684800u    /* 2000-01-01: the RTC's year register holds 00..99 */
#define BOARD_RTC_MAX_UTC 4102444799u   /* 2099-12-31 23:59:59 */

/*
 * The seven time registers (24-hour mode, UTC, year = 2000 + register) to UTC seconds. False when
 * the OS flag is set (the clock stopped, e.g. after a power loss) or any field is out of range, so a
 * garbage RTC never sets the board's clock.
 */
static inline bool board_pcf85063_decode(const uint8_t r[BOARD_PCF85063_TIME_REGS], uint32_t *utc)
{
    board_datetime_t t = {0};
    uint8_t wday, yy;
    if ((r[0] & BOARD_PCF85063_OS) || !board_bcd_field(r[0] & 0x7Fu, 0, 59, &t.sec) ||
        !board_bcd_field(r[1] & 0x7Fu, 0, 59, &t.min) || !board_bcd_field(r[2] & 0x3Fu, 0, 23, &t.hour) ||
        !board_bcd_field(r[3] & 0x3Fu, 1, 31, &t.day) || !board_bcd_field(r[4] & 0x07u, 0, 6, &wday) ||
        !board_bcd_field(r[5] & 0x1Fu, 1, 12, &t.month) || !board_bcd_field(r[6], 0, 99, &yy)) {
        return false;
    }
    t.year = 2000 + yy;
    if (t.day > board_days_in_month(t.year, t.month)) {
        return false;
    }
    *utc = (uint32_t)board_datetime_to_s(&t);
    return true;
}

/* UTC seconds to the seven time registers (OS flag clear). False outside 2000..2099. */
static inline bool board_pcf85063_encode(uint32_t utc, uint8_t r[BOARD_PCF85063_TIME_REGS])
{
    if (utc < BOARD_RTC_MIN_UTC || utc > BOARD_RTC_MAX_UTC) {
        return false;
    }
    board_datetime_t t;
    board_datetime_from_s(utc, &t);
    r[0] = board_bcd(t.sec);
    r[1] = board_bcd(t.min);
    r[2] = board_bcd(t.hour);
    r[3] = board_bcd(t.day);
    r[4] = t.wday;
    r[5] = board_bcd(t.month);
    r[6] = board_bcd((uint8_t)(t.year - 2000));
    return true;
}

/* ---------------------------------------------------------------- humidity sensor: Sensirion SHTC3 */

/* Sensirion's CRC-8 over each 16-bit word: polynomial 0x31, init 0xFF, no reflection, no final XOR. */
static inline uint8_t board_sensirion_crc8(const uint8_t *p, size_t n)
{
    uint8_t crc = 0xFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (uint8_t)(crc & 0x80u ? ((unsigned)crc << 1) ^ 0x31u : (unsigned)crc << 1);
        }
    }
    return crc;
}

/* A "temperature first" read-out (T msb, lsb, crc, RH msb, lsb, crc) to hundredths, rounded.
 * False on a CRC mismatch. T = -45 + 175 * raw / 2^16 degrees C, RH = 100 * raw / 2^16 %. */
static inline bool board_shtc3_decode(const uint8_t b[6], board_climate_t *out)
{
    if (board_sensirion_crc8(b, 2) != b[2] || board_sensirion_crc8(b + 3, 2) != b[5]) {
        return false;
    }
    const uint32_t t = (uint32_t)b[0] << 8 | b[1], rh = (uint32_t)b[3] << 8 | b[4];
    out->temp_centi_c = (int16_t)((int32_t)((17500u * t + 32768u) >> 16) - 4500);
    out->rh_centi_pct = (uint16_t)((10000u * rh + 32768u) >> 16);
    return true;
}

/* ---------------------------------------------------------------- touch: Hynitron CST816S */

#define BOARD_CST816S_DATA_REG 0x02 /* finger count, then X and Y (12 bits each, high nibble first) */
#define BOARD_CST816S_DATA_LEN 5

/* The 5 bytes read from BOARD_CST816S_DATA_REG to a touch point in the controller's coordinates.
 * The upper bits of the X and Y high bytes (event flags) are ignored: a finger count above zero
 * means pressed. Register layout as in Espressif's esp_lcd_touch_cst816s 1.1.2. */
static inline void board_cst816s_decode(const uint8_t b[BOARD_CST816S_DATA_LEN], board_touch_t *out)
{
    out->pressed = b[0] != 0;
    out->x = out->pressed ? (uint16_t)((b[1] & 0x0Fu) << 8 | b[2]) : 0;
    out->y = out->pressed ? (uint16_t)((b[3] & 0x0Fu) << 8 | b[4]) : 0;
}

/* ---------------------------------------------------------------- boot chime (PLAN D89) */

#define BOARD_CHIME_RATE_HZ 16000u
#define BOARD_CHIME_MS 640u
#define BOARD_CHIME_PEAK 9000 /* of 32767: the speaker is small, the amplifier already has gain */

/* Three rising notes (E5, A5, C#6 = an A major arpeggio), each a sine with a 5 ms attack and an exponential-ish
 * decay, so there are no clicks. A pure function of the sample number: nothing to buffer. */
static inline uint32_t board_chime_samples(uint32_t rate_hz) { return rate_hz / 1000u * BOARD_CHIME_MS; }

static inline float board_sine01(float x) /* sine of 2*pi*x, parabolic approximation (error under 0.1 %) */
{
    x -= (float)(int32_t)x;
    x = x < 0.0f ? x + 1.0f : x;
    const float sign = x < 0.5f ? 1.0f : -1.0f;
    x = x < 0.5f ? x : x - 0.5f;
    float y = 16.0f * x * (0.5f - x);
    y = 0.225f * (y * y - y) + y;
    return sign * y;
}

static inline int16_t board_chime_sample(uint32_t i, uint32_t rate_hz)
{
    static const float hz[3] = {659.25f, 880.0f, 1108.73f};
    static const uint32_t start_ms[3] = {0, 140, 280}, len_ms[3] = {260, 260, 360};
    float acc = 0.0f;
    for (int n = 0; n < 3; n++) {
        const uint32_t t0 = start_ms[n] * (rate_hz / 1000u);
        if (i < t0 || i >= t0 + len_ms[n] * (rate_hz / 1000u)) {
            continue;
        }
        const float t = (float)(i - t0) / (float)rate_hz; /* seconds into the note */
        const float attack = t < 0.005f ? t / 0.005f : 1.0f;
        const float left = (float)len_ms[n] / 1000.0f - t;
        const float tail = left < 0.03f ? left / 0.03f : 1.0f; /* the last 30 ms fade out */
        float decay = 1.0f - t * 3.0f;
        decay = decay < 0.15f ? 0.15f : decay;
        acc += board_sine01(hz[n] * t) * attack * tail * decay;
    }
    acc *= 0.6f; /* up to two notes sound at once */
    return (int16_t)(acc * (float)BOARD_CHIME_PEAK);
}

/* ---------------------------------------------------------------- speaker test (PLAN D89, debug only) */

/* Steady tones at 200 Hz to 3.2 kHz, a sweep 200 Hz to 4 kHz, then a C major scale: what the speaker can reproduce.
 * f1 != f0 is a linear sweep. Pure function of the sample number, like the chime. */
typedef struct {
    uint16_t f0, f1, ms;
} board_test_seg_t;

#define BOARD_TEST_SEGS 15
static const board_test_seg_t board_test_seg[BOARD_TEST_SEGS] = {
    {200, 200, 500},  {400, 400, 500},  {800, 800, 500},  {1600, 1600, 500}, {3200, 3200, 500},
    {200, 4000, 2000},
    {523, 523, 250},  {587, 587, 250},  {659, 659, 250},  {698, 698, 250},   {784, 784, 250},
    {880, 880, 250},  {988, 988, 250},  {1047, 1047, 500}, {0, 0, 300},
};

static inline uint32_t board_test_samples(uint32_t rate_hz)
{
    uint32_t ms = 0;
    for (int i = 0; i < BOARD_TEST_SEGS; i++) {
        ms += board_test_seg[i].ms;
    }
    return rate_hz / 1000u * ms;
}

static inline int16_t board_test_sample(uint32_t i, uint32_t rate_hz)
{
    uint32_t t0 = 0;
    for (int n = 0; n < BOARD_TEST_SEGS; n++) {
        const uint32_t len = board_test_seg[n].ms * (rate_hz / 1000u);
        if (i < t0 + len) {
            const board_test_seg_t *g = &board_test_seg[n];
            const float t = (float)(i - t0) / (float)rate_hz, dur = (float)g->ms / 1000.0f;
            const float left = dur - t, edge = t < left ? t : left; /* 10 ms ramps: no clicks between segments */
            const float env = edge < 0.01f ? edge / 0.01f : 1.0f;
            const float cycles = (float)g->f0 * t + (float)((int)g->f1 - (int)g->f0) * t * t / (2.0f * dur);
            return g->f0 == 0 ? 0 : (int16_t)(board_sine01(cycles) * env * (float)BOARD_CHIME_PEAK * 0.6f);
        }
        t0 += len;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif
