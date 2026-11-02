/* board_util.h: the pure helpers of boards that cannot run on the PC (battery curve and level table,
 * key detector, calendar, PCF85063 registers, SHTC3 read-out, CST816S touch point). */
#define _POSIX_C_SOURCE 200809L /* gmtime_r */
#include <string.h>
#include <time.h>

#include "epager/board_util.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------- battery curve */

static void test_batt_filter_ignores_an_outlier(void)
{
    board_batt_filter_t f;
    board_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_INT(3900, board_batt_filter_mv(&f, 3900, 12)); /* the first reading is taken as it is */
    for (int i = 0; i < 4; i++) {
        board_batt_filter_mv(&f, 3900, 12);
    }
    /* a sag of 100 mV during a radio burst, once: the median does not follow it, and it is not a charger */
    TEST_ASSERT_EQUAL_INT(3900, board_batt_filter_mv(&f, 3800, 12));
    TEST_ASSERT_EQUAL_INT(3900, board_batt_filter_mv(&f, 3900, 12));
    TEST_ASSERT_FALSE(f.charging);
}

static void test_batt_filter_follows_a_slow_drift_and_a_big_step(void)
{
    board_batt_filter_t f;
    board_batt_filter_init(&f);
    int last = 0;
    for (int i = 0; i < 60; i++) { /* 60 minutes of discharge, 1 mV each */
        last = board_batt_filter_mv(&f, 3900 - i, 12);
    }
    TEST_ASSERT_INT_WITHIN(8, 3841, last);
    TEST_ASSERT_FALSE(f.charging);
    TEST_ASSERT_EQUAL_INT(4100, board_batt_filter_mv(&f, 4100, 12)); /* +250 mV: taken at once */
    TEST_ASSERT_EQUAL_INT(4100, board_batt_filter_mv(&f, 4100, 12));
}

static void test_batt_charger_plugged_in_and_out(void)
{
    board_batt_filter_t f;
    board_batt_filter_init(&f);
    for (int i = 0; i < 6; i++) {
        board_batt_filter_mv(&f, 4088, 12); /* 86 %, discharging */
    }
    TEST_ASSERT_FALSE(f.charging);
    /* the charger lifts the voltage by 60 mV: seen as a step, the history starts again from it */
    TEST_ASSERT_EQUAL_INT(4148, board_batt_filter_mv(&f, 4148, 30));
    TEST_ASSERT_TRUE(f.charging);
    TEST_ASSERT_EQUAL_INT(4088, board_batt_level_mv(&f, 4148)); /* the level stays where it was */
    for (int i = 0; i < 3; i++) {
        board_batt_filter_mv(&f, 4150, 30);
        TEST_ASSERT_TRUE(f.charging);
    }
    /* unplugged: a step down of 60 mV ends it at once */
    TEST_ASSERT_EQUAL_INT(4090, board_batt_filter_mv(&f, 4090, 12));
    TEST_ASSERT_FALSE(f.charging);
    TEST_ASSERT_EQUAL_INT(4090, board_batt_level_mv(&f, 4090));
}

static void test_batt_charger_seen_at_boot_and_done(void)
{
    board_batt_filter_t f;
    board_batt_filter_init(&f);
    board_batt_filter_mv(&f, 4148, 66); /* the first reading after a boot: the ripple gives it away */
    TEST_ASSERT_TRUE(f.charging);
    for (int i = 0; i < 4; i++) { /* the charge is finishing: quiet readings, but not yet five */
        board_batt_filter_mv(&f, 4160, 12);
        TEST_ASSERT_TRUE(f.charging);
    }
    board_batt_filter_mv(&f, 4160, 12);
    TEST_ASSERT_FALSE(f.charging);
    board_batt_filter_mv(&f, 4160, 12);
    board_batt_filter_mv(&f, 4160, 70); /* ripple again: still plugged in */
    TEST_ASSERT_TRUE(f.charging);
}

static void test_batt_hold_pct(void)
{
    board_batt_filter_t f;
    board_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_UINT8(57, board_batt_hold_pct(&f, 57)); /* the first one is shown */
    TEST_ASSERT_EQUAL_UINT8(57, board_batt_hold_pct(&f, 58)); /* one point of wobble is not shown */
    TEST_ASSERT_EQUAL_UINT8(57, board_batt_hold_pct(&f, 56));
    TEST_ASSERT_EQUAL_UINT8(55, board_batt_hold_pct(&f, 55));
    TEST_ASSERT_EQUAL_UINT8(0, board_batt_hold_pct(&f, 0));   /* the ends are exact */
    TEST_ASSERT_EQUAL_UINT8(100, board_batt_hold_pct(&f, 100));
    TEST_ASSERT_EQUAL_UINT8(100, board_batt_hold_pct(&f, 99));
}

static void test_lipo_clamps(void)
{
    TEST_ASSERT_EQUAL_UINT8(100, board_lipo_pct(4200));
    TEST_ASSERT_EQUAL_UINT8(100, board_lipo_pct(4500));
    TEST_ASSERT_EQUAL_UINT8(0, board_lipo_pct(3270));
    TEST_ASSERT_EQUAL_UINT8(0, board_lipo_pct(3000));
    TEST_ASSERT_EQUAL_UINT8(0, board_lipo_pct(0));
    TEST_ASSERT_EQUAL_UINT8(0, board_lipo_pct(-1));
}

static void test_lipo_full_stretch(void)
{
    /* the charger's end point is 100 %, the table's own top is not needed */
    TEST_ASSERT_EQUAL_UINT8(100, board_lipo_pct_full(4090, 4090));
    TEST_ASSERT_EQUAL_UINT8(100, board_lipo_pct_full(4166, 4090));
    TEST_ASSERT_TRUE(board_lipo_pct_full(4050, 4090) >= 90 && board_lipo_pct_full(4050, 4090) < 100);
    /* below the stretch nothing moves, and a full voltage of 4200 or more is the plain curve */
    TEST_ASSERT_EQUAL_UINT8(board_lipo_pct(3750), board_lipo_pct_full(3750, 4090));
    TEST_ASSERT_EQUAL_UINT8(board_lipo_pct(3610), board_lipo_pct_full(3610, 4090));
    TEST_ASSERT_EQUAL_UINT8(0, board_lipo_pct_full(3270, 4090));
    TEST_ASSERT_EQUAL_UINT8(board_lipo_pct(4150), board_lipo_pct_full(4150, 4200));
    TEST_ASSERT_EQUAL_UINT8(board_lipo_pct(4100), board_lipo_pct_full(4100, 3800)); /* nonsense full: ignored */
    /* never falls as the voltage rises, never exceeds 100 */
    uint8_t last = 0;
    for (int mv = 3000; mv <= 4400; mv++) {
        const uint8_t p = board_lipo_pct_full(mv, 4090);
        TEST_ASSERT_TRUE(p >= last && p <= 100);
        last = p;
    }
}

static void test_lipo_known_points_and_interpolation(void)
{
    TEST_ASSERT_EQUAL_UINT8(50, board_lipo_pct(3840));
    TEST_ASSERT_EQUAL_UINT8(5, board_lipo_pct(3610));
    TEST_ASSERT_EQUAL_UINT8(97, board_lipo_pct(4175)); /* halfway between 4200 (100) and 4150 (95) */
    TEST_ASSERT_EQUAL_UINT8(52, board_lipo_pct(3845)); /* halfway between 3850 (55) and 3840 (50): 52.5 */
}

static void test_lipo_monotonic(void)
{
    uint8_t prev = 0;
    for (int mv = 3000; mv <= 4400; mv++) {
        uint8_t p = board_lipo_pct(mv);
        TEST_ASSERT_TRUE_MESSAGE(p >= prev, "charge level must not fall as the voltage rises");
        TEST_ASSERT_TRUE(p <= 100);
        prev = p;
    }
}

/* ---------------------------------------------------------------- key detector */

#define STEP 10
#define LONG_MS 1000
#define DEB 3

typedef struct {
    board_btn_state_t st;
    int shorts, longs;
} rig_t;

/* Hold the key for `ms` (down) or release it for `ms` (up), counting reported events. */
static void run(rig_t *r, bool down, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += STEP) {
        board_btn_ev_t ev = board_btn_step(&r->st, down, STEP, LONG_MS, DEB);
        r->shorts += ev == BOARD_BTN_EV_SHORT;
        r->longs += ev == BOARD_BTN_EV_LONG;
    }
}

static void test_short_press(void)
{
    rig_t r;
    memset(&r, 0, sizeof(r));
    run(&r, true, 200);
    TEST_ASSERT_EQUAL_INT(0, r.shorts); /* reported on release */
    run(&r, false, 50);
    TEST_ASSERT_EQUAL_INT(1, r.shorts);
    TEST_ASSERT_EQUAL_INT(0, r.longs);
    run(&r, false, 500);
    TEST_ASSERT_EQUAL_INT(1, r.shorts); /* once */
}

static void test_glitch_is_ignored(void)
{
    rig_t r;
    memset(&r, 0, sizeof(r));
    run(&r, true, STEP * (DEB - 1)); /* one sample short of a press */
    run(&r, false, 100);
    TEST_ASSERT_EQUAL_INT(0, r.shorts + r.longs);
    run(&r, true, STEP * DEB); /* exactly the debounce time is a press */
    run(&r, false, STEP);
    TEST_ASSERT_EQUAL_INT(1, r.shorts);
}

static void test_long_press_reports_once_while_held_and_no_short(void)
{
    rig_t r;
    memset(&r, 0, sizeof(r));
    run(&r, true, LONG_MS - 100);
    TEST_ASSERT_EQUAL_INT(0, r.longs);
    run(&r, true, 100 + STEP * DEB); /* the debounce time does not count as held time */
    TEST_ASSERT_EQUAL_INT(1, r.longs);
    run(&r, true, 5000);
    TEST_ASSERT_EQUAL_INT(1, r.longs); /* held on: still once */
    run(&r, false, 100);
    TEST_ASSERT_EQUAL_INT(0, r.shorts); /* releasing after a long press is not a short press */
}

static void test_long_press_timing(void)
{
    rig_t r;
    memset(&r, 0, sizeof(r));
    uint32_t t = 0;
    while (r.longs == 0 && t < 5000) {
        board_btn_ev_t ev = board_btn_step(&r.st, true, STEP, LONG_MS, DEB);
        r.longs += ev == BOARD_BTN_EV_LONG;
        t += STEP;
    }
    /* press recognised after DEB samples, then LONG_MS of held time */
    TEST_ASSERT_EQUAL_UINT32(LONG_MS + STEP * DEB, t);
}

/* PWR on the paper (PLAN D96): LONG at 1.5 s (back), HOLD at 5 s (power off), both while the key is held. */
static void test_hold_follows_long_while_held(void)
{
    board_btn_state_t st;
    memset(&st, 0, sizeof(st));
    int longs = 0, holds = 0, shorts = 0;
    uint32_t t_long = 0, t_hold = 0;
    for (uint32_t t = 0; t < 8000; t += STEP) {
        board_btn_ev_t ev = board_btn_step_hold(&st, true, STEP, 1500, 5000, DEB);
        if (ev == BOARD_BTN_EV_LONG) {
            longs++;
            t_long = t + STEP;
        }
        if (ev == BOARD_BTN_EV_HOLD) {
            holds++;
            t_hold = t + STEP;
        }
        shorts += ev == BOARD_BTN_EV_SHORT;
    }
    TEST_ASSERT_EQUAL_INT(1, longs); /* once each, however long the key stays down */
    TEST_ASSERT_EQUAL_INT(1, holds);
    TEST_ASSERT_EQUAL_UINT32(1500 + STEP * DEB, t_long);
    TEST_ASSERT_EQUAL_UINT32(5000 + STEP * DEB, t_hold);
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_NONE, board_btn_step_hold(&st, false, STEP, 1500, 5000, DEB)); /* release: nothing */
    TEST_ASSERT_EQUAL_INT(0, shorts);
    /* released between the two: LONG only, no HOLD; then a new press starts both over */
    memset(&st, 0, sizeof(st));
    longs = holds = 0;
    for (uint32_t t = 0; t < 3000; t += STEP) {
        board_btn_ev_t ev = board_btn_step_hold(&st, true, STEP, 1500, 5000, DEB);
        longs += ev == BOARD_BTN_EV_LONG;
        holds += ev == BOARD_BTN_EV_HOLD;
    }
    (void)board_btn_step_hold(&st, false, STEP, 1500, 5000, DEB);
    for (uint32_t t = 0; t < 6000; t += STEP) {
        board_btn_ev_t ev = board_btn_step_hold(&st, true, STEP, 1500, 5000, DEB);
        longs += ev == BOARD_BTN_EV_LONG;
        holds += ev == BOARD_BTN_EV_HOLD;
    }
    TEST_ASSERT_EQUAL_INT(2, longs);
    TEST_ASSERT_EQUAL_INT(1, holds);
    /* hold_ms = 0 (BOOT, the LCD keys) never reports HOLD; a short press is still a short press */
    memset(&st, 0, sizeof(st));
    for (uint32_t t = 0; t < 8000; t += STEP) {
        TEST_ASSERT_NOT_EQUAL(BOARD_BTN_EV_HOLD, board_btn_step_hold(&st, true, STEP, 1500, 0, DEB));
    }
    memset(&st, 0, sizeof(st));
    for (uint32_t t = 0; t < 300; t += STEP) {
        (void)board_btn_step_hold(&st, true, STEP, 1500, 5000, DEB);
    }
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_SHORT, board_btn_step_hold(&st, false, STEP, 1500, 5000, DEB));
}

static void test_presses_after_a_long_press(void)
{
    rig_t r;
    memset(&r, 0, sizeof(r));
    run(&r, true, 1500);
    run(&r, false, 100);
    run(&r, true, 100);
    run(&r, false, 100);
    TEST_ASSERT_EQUAL_INT(1, r.longs);
    TEST_ASSERT_EQUAL_INT(1, r.shorts);
}

static void test_counter_saturates(void)
{
    rig_t r;
    memset(&r, 0, sizeof(r));
    run(&r, true, 60000); /* far longer than 255 samples */
    TEST_ASSERT_EQUAL_INT(1, r.longs);
    run(&r, false, 100);
    run(&r, true, 100);
    run(&r, false, 100);
    TEST_ASSERT_EQUAL_INT(1, r.shorts); /* still detects the next press */
}

/* ---------------------------------------------------------------- calendar */

static void test_datetime_matches_gmtime(void)
{
    /* every 7 hours 13 minutes from 1970 to 2106 (u32 UTC range), against the C library */
    for (int64_t s = 0; s <= 4294967295LL; s += 7 * 3600 + 13 * 60 + 7) {
        time_t tt = (time_t)s;
        struct tm g;
        gmtime_r(&tt, &g);
        board_datetime_t t;
        board_datetime_from_s(s, &t);
        if (t.year != g.tm_year + 1900 || t.month != g.tm_mon + 1 || t.day != g.tm_mday || t.hour != g.tm_hour ||
            t.min != g.tm_min || t.sec != g.tm_sec || t.wday != g.tm_wday || board_datetime_to_s(&t) != s) {
            TEST_FAIL_MESSAGE("mismatch");
        }
    }
}

static void test_datetime_negative_and_known(void)
{
    board_datetime_t t;
    board_datetime_from_s(-1, &t); /* local time before the epoch (UTC 0 with a negative offset) */
    TEST_ASSERT_EQUAL_INT(1969, t.year);
    TEST_ASSERT_EQUAL_UINT8(12, t.month);
    TEST_ASSERT_EQUAL_UINT8(31, t.day);
    TEST_ASSERT_EQUAL_UINT8(23, t.hour);
    TEST_ASSERT_EQUAL_UINT8(59, t.sec);
    TEST_ASSERT_EQUAL_UINT8(3, t.wday); /* Wednesday */
    TEST_ASSERT_EQUAL_INT64(-1, board_datetime_to_s(&t));
    board_datetime_from_s(1791728825, &t);
    TEST_ASSERT_EQUAL_INT(2026, t.year);
    TEST_ASSERT_EQUAL_UINT8(10, t.month);
    TEST_ASSERT_EQUAL_UINT8(11, t.day);
    TEST_ASSERT_EQUAL_UINT8(14, t.hour);
    TEST_ASSERT_EQUAL_UINT8(27, t.min);
    TEST_ASSERT_EQUAL_UINT8(5, t.sec);
    TEST_ASSERT_EQUAL_UINT8(0, t.wday); /* Sunday */
}

static void test_days_in_month(void)
{
    TEST_ASSERT_EQUAL_UINT8(29, board_days_in_month(2000, 2));
    TEST_ASSERT_EQUAL_UINT8(29, board_days_in_month(2024, 2));
    TEST_ASSERT_EQUAL_UINT8(28, board_days_in_month(2026, 2));
    TEST_ASSERT_EQUAL_UINT8(28, board_days_in_month(2100, 2));
    TEST_ASSERT_EQUAL_UINT8(31, board_days_in_month(2026, 12));
    TEST_ASSERT_EQUAL_UINT8(30, board_days_in_month(2026, 4));
}

/* ---------------------------------------------------------------- PCF85063 */

static void test_pcf85063_known_registers(void)
{
    uint8_t r[BOARD_PCF85063_TIME_REGS];
    TEST_ASSERT_TRUE(board_pcf85063_encode(1791728825, r)); /* 2026-10-11 14:27:05, Sunday */
    const uint8_t want[] = {0x05, 0x27, 0x14, 0x11, 0x00, 0x10, 0x26};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, r, sizeof(want));
    uint32_t utc = 0;
    TEST_ASSERT_TRUE(board_pcf85063_decode(r, &utc));
    TEST_ASSERT_EQUAL_UINT32(1791728825, utc);
}

static void test_pcf85063_round_trip(void)
{
    for (uint32_t s = BOARD_RTC_MIN_UTC; s <= BOARD_RTC_MAX_UTC - 86399u; s += 86399u) {
        uint8_t r[BOARD_PCF85063_TIME_REGS];
        uint32_t back = 0;
        if (!board_pcf85063_encode(s, r) || !board_pcf85063_decode(r, &back) || back != s) {
            TEST_FAIL_MESSAGE("round trip");
        }
    }
    uint8_t r[BOARD_PCF85063_TIME_REGS];
    uint32_t back = 0;
    TEST_ASSERT_TRUE(board_pcf85063_encode(BOARD_RTC_MAX_UTC, r));
    TEST_ASSERT_TRUE(board_pcf85063_decode(r, &back));
    TEST_ASSERT_EQUAL_UINT32(BOARD_RTC_MAX_UTC, back);
    TEST_ASSERT_FALSE(board_pcf85063_encode(BOARD_RTC_MIN_UTC - 1u, r));
    TEST_ASSERT_FALSE(board_pcf85063_encode(BOARD_RTC_MAX_UTC + 1u, r));
}

static void test_pcf85063_rejects_invalid(void)
{
    /* reset state: OS set, 2000-01-01 00:00:00 Saturday */
    const uint8_t reset[] = {0x80, 0x00, 0x00, 0x01, 0x06, 0x01, 0x00};
    uint32_t utc = 123;
    TEST_ASSERT_FALSE(board_pcf85063_decode(reset, &utc));
    uint8_t r[] = {0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x00};
    TEST_ASSERT_TRUE(board_pcf85063_decode(r, &utc));
    TEST_ASSERT_EQUAL_UINT32(BOARD_RTC_MIN_UTC, utc);
    const struct { uint8_t reg, val; } bad[] = {
        {0, 0x60}, {0, 0x0A}, {1, 0x60}, {2, 0x24}, {3, 0x00}, {3, 0x32}, {4, 0x07}, {5, 0x00}, {5, 0x13},
        {6, 0xA0}, {6, 0x9A},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t t[sizeof(r)];
        memcpy(t, r, sizeof(r));
        t[bad[i].reg] = bad[i].val;
        TEST_ASSERT_FALSE_MESSAGE(board_pcf85063_decode(t, &utc), "field out of range accepted");
    }
    uint8_t feb30[] = {0x00, 0x00, 0x00, 0x30, 0x00, 0x02, 0x26};
    TEST_ASSERT_FALSE(board_pcf85063_decode(feb30, &utc));
    uint8_t feb29[] = {0x00, 0x00, 0x00, 0x29, 0x00, 0x02, 0x28}; /* 2028 is a leap year */
    TEST_ASSERT_TRUE(board_pcf85063_decode(feb29, &utc));
    feb29[6] = 0x27;
    TEST_ASSERT_FALSE(board_pcf85063_decode(feb29, &utc));
    /* unused bits above each field are ignored */
    uint8_t noisy[] = {0x00, 0x80, 0xC0, 0xC1, 0xF8, 0xE1, 0x00};
    TEST_ASSERT_TRUE(board_pcf85063_decode(noisy, &utc));
    TEST_ASSERT_EQUAL_UINT32(BOARD_RTC_MIN_UTC, utc);
}

/* ---------------------------------------------------------------- SHTC3 */

static void test_sensirion_crc8(void)
{
    const uint8_t zero = 0x00, beef[] = {0xBE, 0xEF};
    TEST_ASSERT_EQUAL_HEX8(0xAC, board_sensirion_crc8(&zero, 1)); /* SHTC3 datasheet 5.6 */
    TEST_ASSERT_EQUAL_HEX8(0x92, board_sensirion_crc8(beef, 2));
}

static void test_shtc3_decode(void)
{
    /* datasheet figure 7 example, reordered T first: 23.7 degrees C, 63 % */
    const uint8_t ex[] = {0x64, 0x8B, 0xC7, 0xA1, 0x33, 0x1C};
    board_climate_t c;
    TEST_ASSERT_TRUE(board_shtc3_decode(ex, &c));
    TEST_ASSERT_EQUAL_INT16(2373, c.temp_centi_c); /* -45 + 175 * 25739 / 65536 = 23.73 */
    TEST_ASSERT_EQUAL_UINT16(6297, c.rh_centi_pct); /* 100 * 41267 / 65536 = 62.97 */
    const uint8_t lo[] = {0x00, 0x00, 0x81, 0x00, 0x00, 0x81}, hi[] = {0xFF, 0xFF, 0xAC, 0xFF, 0xFF, 0xAC};
    TEST_ASSERT_TRUE(board_shtc3_decode(lo, &c));
    TEST_ASSERT_EQUAL_INT16(-4500, c.temp_centi_c);
    TEST_ASSERT_EQUAL_UINT16(0, c.rh_centi_pct);
    TEST_ASSERT_TRUE(board_shtc3_decode(hi, &c));
    TEST_ASSERT_EQUAL_INT16(13000, c.temp_centi_c);
    TEST_ASSERT_EQUAL_UINT16(10000, c.rh_centi_pct);
    uint8_t bad[sizeof(ex)];
    memcpy(bad, ex, sizeof(ex));
    bad[2] ^= 1;
    TEST_ASSERT_FALSE(board_shtc3_decode(bad, &c));
    memcpy(bad, ex, sizeof(ex));
    bad[4] ^= 1;
    TEST_ASSERT_FALSE(board_shtc3_decode(bad, &c));
}

/* ---------------------------------------------------------------- key chord */

/* hold 3 s; tap 150 ms .. 1.5 s */
static board_btn_ev_t chord(board_chord_state_t *s, bool a, bool b)
{
    return board_chord_step(s, a, b, 10, 3000, 150, 1500);
}

/* Both keys down for `ms`, then both up: the event of the release. */
static board_btn_ev_t chord_press(board_chord_state_t *s, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_NONE, chord(s, true, true));
    }
    return chord(s, false, false);
}

static void test_chord(void)
{
    board_chord_state_t s = {0};
    for (int i = 0; i < 50; i++) { /* one key alone: never a chord */
        TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_NONE, chord(&s, true, false));
    }
    TEST_ASSERT_FALSE(s.active);
    int fired = 0, at = -1;
    for (int i = 0; i < 400; i++) { /* both held for 4 s: LONG once, at 3 s */
        const board_btn_ev_t ev = chord(&s, true, true);
        if (ev != BOARD_BTN_EV_NONE) {
            TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_LONG, ev);
            fired++;
            at = (i + 1) * 10;
        }
        TEST_ASSERT_TRUE(s.active);
    }
    TEST_ASSERT_EQUAL_INT(1, fired);
    TEST_ASSERT_EQUAL_INT(3000, at);
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_NONE, chord(&s, false, true)); /* one released: still active */
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_NONE, chord(&s, false, false)); /* the long chord is over: no tap */
    TEST_ASSERT_FALSE(s.active);
    for (int i = 0; i < 299; i++) { /* a new chord counts from zero */
        TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_NONE, chord(&s, true, true));
    }
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_LONG, chord(&s, true, true));
    chord(&s, false, false);
}

static void test_chord_tap(void)
{
    board_chord_state_t s = {0};
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_SHORT, chord_press(&s, 300)); /* a tap: reported on release */
    TEST_ASSERT_FALSE(s.active);
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_NONE, chord_press(&s, 100)); /* a key roll-over: nothing */
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_SHORT, chord_press(&s, 150)); /* the limits are inclusive */
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_SHORT, chord_press(&s, 1500));
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_NONE, chord_press(&s, 1600)); /* neither a tap nor the 3 s chord */
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_NONE, chord_press(&s, 2900));
    /* one key lifts first, then the other: still one tap */
    for (int i = 0; i < 30; i++) {
        chord(&s, true, true);
    }
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_NONE, chord(&s, false, true));
    }
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_SHORT, chord(&s, false, false));
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_NONE, chord(&s, false, false)); /* once */
    /* a single key never taps */
    for (int i = 0; i < 50; i++) {
        chord(&s, true, false);
    }
    TEST_ASSERT_EQUAL_INT(BOARD_BTN_EV_NONE, chord(&s, false, false));
}

/* ---------------------------------------------------------------- level table, CST816S */

/* The ostb-xiaozhi-3st stock table (raw ADC counts on battery), docs/ostb-xiaozhi-3st.md. */
static const uint16_t RAW[] = {1890, 1950, 2010, 2070, 2130, 2190, 2250, 2310, 2370, 2430, 2460, 2489};
static const uint8_t PCT[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 100};

static void test_table_pct_points_clamps_interpolation(void)
{
    const size_t n = sizeof(RAW) / sizeof(RAW[0]);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT8(PCT[i], board_table_pct(RAW[i], RAW, PCT, n));
    }
    TEST_ASSERT_EQUAL_UINT8(0, board_table_pct(0, RAW, PCT, n));
    TEST_ASSERT_EQUAL_UINT8(0, board_table_pct(-5, RAW, PCT, n));
    TEST_ASSERT_EQUAL_UINT8(100, board_table_pct(4095, RAW, PCT, n));
    TEST_ASSERT_EQUAL_UINT8(5, board_table_pct(1920, RAW, PCT, n));  /* halfway between 0 and 10 */
    TEST_ASSERT_EQUAL_UINT8(92, board_table_pct(2445, RAW, PCT, n)); /* halfway between 90 and 95: 92.5 */
    uint8_t prev = 0;
    for (int x = 1800; x <= 2600; x++) {
        uint8_t p = board_table_pct(x, RAW, PCT, n);
        TEST_ASSERT_TRUE(p >= prev && p <= 100);
        prev = p;
    }
}

static void test_cst816s_decode(void)
{
    board_touch_t t;
    const uint8_t none[] = {0x00, 0x01, 0x23, 0x00, 0x45};
    board_cst816s_decode(none, &t);
    TEST_ASSERT_FALSE(t.pressed);
    TEST_ASSERT_EQUAL_UINT16(0, t.x);
    const uint8_t one[] = {0x01, 0x81, 0x27, 0x40, 0xEF}; /* event flags in the high bits are ignored */
    board_cst816s_decode(one, &t);
    TEST_ASSERT_TRUE(t.pressed);
    TEST_ASSERT_EQUAL_UINT16(0x127, t.x); /* 295 */
    TEST_ASSERT_EQUAL_UINT16(0x0EF, t.y); /* 239 */
    const uint8_t max[] = {0x02, 0xFF, 0xFF, 0xFF, 0xFF};
    board_cst816s_decode(max, &t);
    TEST_ASSERT_TRUE(t.pressed);
    TEST_ASSERT_EQUAL_UINT16(0xFFF, t.x);
    TEST_ASSERT_EQUAL_UINT16(0xFFF, t.y);
}

/* ---------------------------------------------------------------- boot chime */

static void test_chime(void)
{
    const uint32_t n = board_chime_samples(BOARD_CHIME_RATE_HZ);
    TEST_ASSERT_EQUAL_UINT32(10240, n);
    int peak = 0;
    int16_t prev = 0;
    int maxstep = 0;
    for (uint32_t i = 0; i < n; i++) {
        const int16_t v = board_chime_sample(i, BOARD_CHIME_RATE_HZ);
        peak = v > peak ? v : peak;
        peak = -v > peak ? -v : peak;
        const int step = v > prev ? v - prev : prev - v;
        maxstep = step > maxstep ? step : maxstep;
        prev = v;
    }
    TEST_ASSERT_TRUE(peak > BOARD_CHIME_PEAK / 4 && peak < BOARD_CHIME_PEAK); /* audible, never clipped */
    TEST_ASSERT_TRUE(maxstep < BOARD_CHIME_PEAK / 2);                         /* no click: a jump would be about the peak */
    TEST_ASSERT_EQUAL_INT(0, board_chime_sample(0, BOARD_CHIME_RATE_HZ));     /* starts and ends silent */
    TEST_ASSERT_EQUAL_INT(0, board_chime_sample(n - 1, BOARD_CHIME_RATE_HZ));
}

static void test_sine_approximation(void)
{
    for (int i = 0; i <= 100; i++) {
        const float x = (float)i / 100.0f - 0.37f;
        const float ref = (float)__builtin_sin(6.283185307 * (double)x);
        const float d = board_sine01(x) - ref;
        TEST_ASSERT_TRUE(d < 0.002f && d > -0.002f);
    }
}

static void test_speaker_test_sound(void)
{
    const uint32_t n = board_test_samples(BOARD_CHIME_RATE_HZ);
    TEST_ASSERT_EQUAL_UINT32(16u * 7050u, n); /* 7.05 s */
    int peak = 0, zero_run = 0;
    for (uint32_t i = 0; i < n; i++) {
        const int v = board_test_sample(i, BOARD_CHIME_RATE_HZ);
        peak = v > peak ? v : peak;
        peak = -v > peak ? -v : peak;
        zero_run = v == 0 ? zero_run + 1 : 0;
    }
    TEST_ASSERT_TRUE(peak > BOARD_CHIME_PEAK / 2 && peak <= BOARD_CHIME_PEAK);
    TEST_ASSERT_EQUAL_INT(0, board_test_sample(0, BOARD_CHIME_RATE_HZ));
    TEST_ASSERT_EQUAL_INT(0, board_test_sample(n - 1, BOARD_CHIME_RATE_HZ));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_batt_filter_ignores_an_outlier);
    RUN_TEST(test_batt_filter_follows_a_slow_drift_and_a_big_step);
    RUN_TEST(test_batt_charger_plugged_in_and_out);
    RUN_TEST(test_batt_charger_seen_at_boot_and_done);
    RUN_TEST(test_batt_hold_pct);
    RUN_TEST(test_lipo_clamps);
    RUN_TEST(test_lipo_full_stretch);
    RUN_TEST(test_lipo_known_points_and_interpolation);
    RUN_TEST(test_lipo_monotonic);
    RUN_TEST(test_short_press);
    RUN_TEST(test_glitch_is_ignored);
    RUN_TEST(test_long_press_reports_once_while_held_and_no_short);
    RUN_TEST(test_long_press_timing);
    RUN_TEST(test_hold_follows_long_while_held);
    RUN_TEST(test_presses_after_a_long_press);
    RUN_TEST(test_counter_saturates);
    RUN_TEST(test_datetime_matches_gmtime);
    RUN_TEST(test_datetime_negative_and_known);
    RUN_TEST(test_days_in_month);
    RUN_TEST(test_pcf85063_known_registers);
    RUN_TEST(test_pcf85063_round_trip);
    RUN_TEST(test_pcf85063_rejects_invalid);
    RUN_TEST(test_sensirion_crc8);
    RUN_TEST(test_shtc3_decode);
    RUN_TEST(test_table_pct_points_clamps_interpolation);
    RUN_TEST(test_cst816s_decode);
    RUN_TEST(test_chord);
    RUN_TEST(test_chord_tap);
    RUN_TEST(test_chime);
    RUN_TEST(test_sine_approximation);
    RUN_TEST(test_speaker_test_sound);
    return UNITY_END();
}
