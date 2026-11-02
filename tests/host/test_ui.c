/* ui_common on the simulated Paper: refresh policy, coalescing, unchanged frames, panel sleep,
 * the placeholder card model, buttons, languages, the idle clock and climate screen. Pixels are
 * checked by the golden-image tests. */
#include <string.h>

#include "epager/proto_gen.h"
#include "epager/blob_storage.h"
#include "epager/sim_epaper.h"
#include "epager/ui.h"
#include "epager/xfer.h"
#include "ui_icons.h"
#include "unity.h"
#include "lvgl.h"

LV_FONT_DECLARE(ui_font_regular_16)

#define ICON_X 4 /* top-left of the card icon */
#define ICON_Y 26 /* BAR_H + GAP */

static uint32_t now;
static blob_mem_t blob;
static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return now;
}

#define T_14_27_05 1791728825u /* Sunday 2026-10-11 14:27:05 UTC */

static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        now += 10;
        sim_epaper_tick(now);
        (void)ui_service();
    }
}

static ui_config_t cfg(uint8_t full_every)
{
    return (ui_config_t){.language = PROTO_LANGUAGE_EN, .show_body = true, .full_refresh_every = full_every,
                         .clock_24h = true};
}

static void start(bool climate)
{
    ui_params_t p = {.display = board_get()->display, .blob = &blob.iface, .climate = climate, .now_ms = now_ms};
    TEST_ASSERT_TRUE(ui_init(&p));
    ui_config_t c = cfg(10);
    ui_set_config(&c);
}

void setUp(void)
{
    now = 1000;
    sim_epaper_reset();
    blob_mem_init(&blob, 8192);
    start(false);
}

void tearDown(void)
{
    ui_deinit();
    blob_mem_free(&blob);
}

static npipe_notif_t notif(uint32_t id, const char *app, const char *title, const char *body)
{
    npipe_notif_t n;
    memset(&n, 0, sizeof(n));
    n.id = id;
    n.kind = NPIPE_ADDED;
    n.shown = true;
    snprintf(n.app_name, sizeof(n.app_name), "%s", app);
    snprintf(n.title, sizeof(n.title), "%s", title);
    snprintf(n.body, sizeof(n.body), "%s", body);
    n.app_name_len = (uint16_t)strlen(n.app_name);
    n.title_len = (uint16_t)strlen(n.title);
    n.body_len = (uint16_t)strlen(n.body);
    return n;
}

static const char *text(void)
{
    static char t[UI_TEXT_MAX];
    ui_visible_text(t, sizeof(t));
    return t;
}

static sim_epaper_state_t state(void)
{
    sim_epaper_state_t s;
    sim_epaper_state(&s);
    return s;
}

static bool pixel_black(uint32_t x, uint32_t y)
{
    return !((sim_epaper_frame()[y * SIM_EPAPER_STRIDE + x / 8] >> (7 - x % 8)) & 1);
}

static void test_boot_is_one_full_refresh(void)
{
    run(100);
    TEST_ASSERT_TRUE(state().refreshing);
    TEST_ASSERT_EQUAL_UINT8(BOARD_REFRESH_FULL, state().kind);
    TEST_ASSERT_FALSE(pixel_black(4, 50)); /* not shown before the refresh completes */
    run(2000);
    TEST_ASSERT_FALSE(state().refreshing);
    TEST_ASSERT_EQUAL_UINT32(1, state().full);
    TEST_ASSERT_EQUAL_UINT32(0, state().partial);
    TEST_ASSERT_TRUE(pixel_black(4, 50));     /* the clock box's left border */
    TEST_ASSERT_FALSE(pixel_black(100, 0));   /* no header on the idle screen */
    TEST_ASSERT_FALSE(pixel_black(100, 150)); /* empty body */
    TEST_ASSERT_TRUE(state().asleep);         /* panel asleep between refreshes */
    TEST_ASSERT_EQUAL_STRING("--:--\nTime not set\n\n\n", text());
    TEST_ASSERT_EQUAL_UINT32(0, state().violations);
}

/* One clock redraw: the minute changes, then `quiet_ms` pass (a full refresh waits for 60 quiet seconds). */
static char clock_minute(uint32_t *utc, uint32_t quiet_ms)
{
    *utc += 60;
    ui_set_time(*utc);
    run(quiet_ms);
    return state().kind == BOARD_REFRESH_FULL ? 'F' : 'P';
}

static void test_full_refresh_every(void)
{
    ui_config_t c = cfg(3);
    ui_set_config(&c);
    uint32_t utc = T_14_27_05;
    ui_set_time(utc);
    run(61000); /* the boot refresh, then the quiet minute */
    char kinds[16] = {0};
    for (uint32_t i = 0; i < 8; i++) {
        kinds[i] = clock_minute(&utc, 61000);
    }
    TEST_ASSERT_EQUAL_STRING("PPPFPPPF", kinds); /* a clock redraw counts as one partial */
    ui_stats_t s;
    ui_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(3, s.full);
    TEST_ASSERT_EQUAL_UINT32(6, s.partial);
    TEST_ASSERT_EQUAL_UINT32(0, state().violations);
}

static void test_full_refresh_cards_count_a_quarter(void)
{
    ui_config_t c = cfg(3);
    ui_set_config(&c);
    run(2100);
    for (uint32_t i = 0; i < 11; i++) { /* 11 quarters: two partials' worth, below 3 */
        char t[16];
        snprintf(t, sizeof(t), "Message %u", (unsigned)i);
        npipe_notif_t n = notif(i + 1, "Zalo", t, "");
        ui_on_notification(&n);
        run(4200);
        TEST_ASSERT_EQUAL_UINT8(BOARD_REFRESH_PARTIAL, state().kind);
    }
    ui_stats_t s;
    ui_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(1, s.full); /* the boot refresh only */
    TEST_ASSERT_EQUAL_UINT32(2, s.partials_since_full);
}

static void test_full_refresh_waits_for_a_quiet_moment(void)
{
    ui_config_t c = cfg(2);
    ui_set_config(&c);
    uint32_t utc = T_14_27_05;
    ui_set_time(utc);
    run(61000);
    char kinds[16] = {0};
    kinds[0] = clock_minute(&utc, 61000); /* count 1 */
    kinds[1] = clock_minute(&utc, 61000); /* count 2: due, nobody is using the board */
    kinds[2] = clock_minute(&utc, 61000);
    TEST_ASSERT_EQUAL_STRING("PPF", kinds);
    /* in use: a key press a moment before every redraw. The full refresh is due at count 2, waits, and is
     * taken at count 4 (twice the setting) at the latest */
    for (uint32_t i = 0; i < 6; i++) {
        ui_activity();
        kinds[i] = clock_minute(&utc, 10000);
        ui_activity();
        run(1);
    }
    TEST_ASSERT_EQUAL_STRING("PPPPFP", kinds);
}

static void test_partial_takes_300ms(void)
{
    run(2100);
    npipe_notif_t n = notif(1, "Zalo", "Hi", "");
    ui_on_notification(&n);
    run(200);
    TEST_ASSERT_TRUE(state().refreshing);
    TEST_ASSERT_EQUAL_UINT8(BOARD_REFRESH_PARTIAL, state().kind);
    run(150);
    TEST_ASSERT_FALSE(state().refreshing);
    TEST_ASSERT_EQUAL_UINT32(1, state().partial);
}

static void test_changes_during_a_refresh_are_coalesced(void)
{
    run(100); /* boot refresh running */
    for (uint32_t i = 1; i <= 3; i++) { /* in one tick: nothing is on the panel yet, so each opens at once */
        npipe_notif_t n = notif(i, "App", i == 3 ? "Last" : "Early", "");
        ui_on_notification(&n);
    }
    run(2500);
    TEST_ASSERT_EQUAL_UINT32(1, state().full);
    TEST_ASSERT_EQUAL_UINT32(1, state().partial); /* one refresh for three changes */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Last"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "1/3"));
}

static void test_unchanged_frame_is_not_refreshed(void)
{
    npipe_notif_t n = notif(1, "Zalo", "Same", "text");
    ui_on_notification(&n);
    run(2100);
    n.kind = NPIPE_MODIFIED;
    ui_on_notification(&n);
    run(500);
    ui_stats_t s;
    ui_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(1, s.skipped);
    TEST_ASSERT_EQUAL_UINT32(0, state().partial);
}

/* The same picture turned clockwise by `turns` quarters: reference for the rotation test. */
static bool turned_black(const uint8_t *frame0, uint32_t turns, uint32_t dx, uint32_t dy)
{
    const uint32_t n = 200; /* the panel is square */
    uint32_t sx = dx, sy = dy;
    if (turns == 1) {
        sx = dy;
        sy = n - 1 - dx;
    } else if (turns == 2) {
        sx = n - 1 - dx;
        sy = n - 1 - dy;
    } else if (turns == 3) {
        sx = n - 1 - dy;
        sy = dx;
    }
    return !((frame0[sy * SIM_EPAPER_STRIDE + sx / 8] >> (7 - sx % 8)) & 1);
}

static void test_orientation_turns_the_picture(void)
{
    npipe_notif_t n = notif(1, "Zalo", "Rotate me", "an asymmetric picture");
    ui_on_notification(&n);
    run(2100);
    uint8_t frame0[SIM_EPAPER_STRIDE * 200];
    memcpy(frame0, sim_epaper_frame(), sizeof(frame0));
    TEST_ASSERT_TRUE(pixel_black(4, 4)); /* the header bar's corner: not symmetric under any turn */
    ui_config_t c = cfg(10);
    const uint8_t order[] = {1, 2, 3, 0};
    for (size_t i = 0; i < sizeof(order); i++) {
        c.orientation = order[i];
        ui_set_config(&c);
        run(2100);
        for (uint32_t y = 0; y < 200; y++) {
            for (uint32_t x = 0; x < 200; x++) {
                if (pixel_black(x, y) != turned_black(frame0, order[i], x, y)) {
                    char msg[48];
                    snprintf(msg, sizeof(msg), "turns=%u x=%u y=%u", order[i], (unsigned)x, (unsigned)y);
                    TEST_FAIL_MESSAGE(msg);
                }
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, state().violations);
    /* a value above 3 is the same as its remainder: the registry keeps it to 0-3, the UI must not read past */
    c.orientation = 5;
    ui_set_config(&c);
    run(2100);
    TEST_ASSERT_FALSE(pixel_black(4, 4) != turned_black(frame0, 1, 4, 4));
}

static void test_badge_and_clock_keys(void)
{
    ui_set_time(1791700215);
    run(400);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT)); /* nothing to show */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG));  /* long BOOT: nothing */
    npipe_notif_t a = notif(1, "Zalo", "First", "a"), b = notif(2, "Mail", "Second", "b");
    ui_on_notification(&a);
    ui_on_notification(&b);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail\n1/2"));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG));
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail\n1/2")); /* a long press changes nothing */
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT); /* newer than the newest card: the clock */
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Second"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "\n2\n")); /* the badge counts the two waiting notifications */
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT); /* short PWR on the clock: the newest card */
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail\n1/2"));
    /* a dismissal at the source empties the clock's badge */
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    b.kind = NPIPE_REMOVED;
    a.kind = NPIPE_REMOVED;
    ui_on_notification(&a);
    ui_on_notification(&b);
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "\n2\n"));
    TEST_ASSERT_NULL(strstr(text(), "\n1\n"));
    /* more than the UI keeps: the badge shows the kept cards */
    for (uint32_t i = 1; i <= 10; i++) {
        npipe_notif_t n = notif(i, "App", "T", "");
        ui_on_notification(&n);
    }
    run(2100);
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "\n8\n"));
}

static uint32_t black_in(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint32_t n = 0;
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            n += pixel_black(x, y) ? 1u : 0u;
        }
    }
    return n;
}

static void test_setup_screen_qr(void)
{
    ui_params_t p = {.display = board_get()->display, .blob = &blob.iface, .now_ms = now_ms, .wifi_setup = true};
    ui_deinit();
    TEST_ASSERT_TRUE(ui_init(&p));
    ui_config_t c = cfg(10);
    ui_set_config(&c);
    ui_wifi_setup_t st = {.state = UI_SETUP_WAITING};
    snprintf(st.ap_ssid, sizeof(st.ap_ssid), "ePager-1A2B");
    snprintf(st.ap_password, sizeof(st.ap_password), "abcdefghij");
    snprintf(st.url, sizeof(st.url), "192.168.4.1");
    ui_set_wifi_setup(&st);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Join ePager-1A2B\nPassword abcdefghij\nthen open 192.168.4.1\nAny key: close\n"));
    const uint32_t qr = black_in(50, 4, 150, 104); /* the code's canvas */
    TEST_ASSERT_TRUE_MESSAGE(qr > 2000 && qr < 8000, "a QR code is drawn");
    TEST_ASSERT_EQUAL_UINT32(0, black_in(0, 0, 50, 104)); /* nothing outside the code in its rows */
    /* the outcome screens have no code */
    st.state = UI_SETUP_JOINING;
    snprintf(st.network, sizeof(st.network), "Home");
    ui_set_wifi_setup(&st);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Connecting to\nHome ...\n"));
    TEST_ASSERT_EQUAL_UINT32(0, black_in(50, 4, 150, 50));
    /* any key closes it */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_WIFI_SETUP_CLOSE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_WIFI_SETUP_CLOSE, ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT));
    st.state = UI_SETUP_OFF;
    ui_set_wifi_setup(&st);
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Any key"));
}

static void test_weather_row(void)
{
    ui_deinit();
    start(true);
    ui_set_time(T_14_27_05);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Sunday, 11 Oct\n--.-°C\n--%\n")); /* no weather: no row */
    wx_reading_t r = {.temp_c10 = 312, .high_c10 = 321, .low_c10 = 246, .precip_pct = 86, .cond = WX_COND_CLEAR,
                      .is_day = true};
    ui_set_weather(&r);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Sunday, 11 Oct\n32°/25°\n86%\n--.-°C\n--%\n"));
    /* the weather icon is the first thing in the row: the sun, left of the high / low */
    uint32_t rows = 0;
    for (uint32_t y = 60; y < 130; y++) {
        for (uint32_t x = 20; x < 60; x++) {
            rows += pixel_black(x, y) ? 1u : 0u;
        }
    }
    TEST_ASSERT_TRUE(rows > 100);
    /* a reading without high / low or rain: just the icon (and nothing for the missing parts) */
    r.high_c10 = r.low_c10 = WX_TEMP_UNKNOWN;
    r.precip_pct = WX_PCT_UNKNOWN;
    r.cond = WX_COND_THUNDER;
    ui_set_weather(&r);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Sunday, 11 Oct\n--.-°C\n--%\n"));
    TEST_ASSERT_NULL(strstr(text(), "%\n--.-"));
    /* Fahrenheit applies to the high / low as well */
    r.high_c10 = 321;
    r.low_c10 = 246;
    ui_set_weather(&r);
    ui_config_t c = cfg(10);
    c.temp_unit = PROTO_TEMP_UNIT_FAHRENHEIT;
    ui_set_config(&c);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "90°/76°\n"));
    /* the cards take the screen, and no weather behind them */
    npipe_notif_t n = notif(1, "Zalo", "Hi", "");
    ui_on_notification(&n);
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "90"));
    ui_set_weather(NULL);
}

static void test_pairing_screen_and_keys(void)
{
    ui_params_t p = {.display = board_get()->display, .blob = &blob.iface, .now_ms = now_ms, .wifi_setup = true};
    ui_deinit();
    TEST_ASSERT_TRUE(ui_init(&p));
    ui_config_t c = cfg(10);
    ui_set_config(&c);
    npipe_notif_t n = notif(1, "Zalo", "Hello", "Body");
    ui_on_notification(&n);
    ui_status_t st = {.battery_pct = 57, .pairing_mode = true, .ota_pct = -1};
    ui_set_status(&st);
    run(2100);
    /* the whole screen is the pairing screen: no card, no clock */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Pairing mode\nOpen the ePager app"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "BOOT: Wi-Fi setup\nHold BOOT+PWR: cancel\n"));
    TEST_ASSERT_NULL(strstr(text(), "Hello"));
    /* short BOOT opens the Wi-Fi setup; the other keys do nothing here */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_WIFI_SETUP, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_PAIRING_TOGGLE, ui_on_button(BOARD_BTN_BOOT_PWR, BOARD_PRESS_LONG));
    /* a phone entering the code: the code only, no key does anything */
    st.has_passkey = true;
    st.passkey = 42;
    ui_set_status(&st);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "000042"));
    TEST_ASSERT_NULL(strstr(text(), "BOOT: Wi-Fi setup"));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT));
    /* pairing over: the card (a notification came in meanwhile) is back */
    st.has_passkey = false;
    st.pairing_mode = false;
    ui_set_status(&st);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Hello"));
    /* a board without Wi-Fi does not offer the setup */
    ui_deinit();
    p.wifi_setup = false;
    TEST_ASSERT_TRUE(ui_init(&p));
    ui_set_config(&c);
    st.pairing_mode = true;
    ui_set_status(&st);
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Wi-Fi setup"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Hold BOOT+PWR: cancel"));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT));
}

static void test_wifi_status_icon(void)
{
    ui_set_time(T_14_27_05);
    ui_status_t st = {.battery_pct = 57, .connected = true, .ota_pct = -1};
    ui_set_status(&st);
    run(2100);
    TEST_ASSERT_EQUAL_INT(UI_WIFI_NONE, ui_wifi_icon()); /* no network stored: no icon (nor while the radio is off) */
    const uint32_t bare = black_in(100, 176, 200, 200);
    static const struct { uint8_t state; ui_wifi_icon_t icon; } cases[] = {
        {PROTO_WIFI_STATE_IDLE, UI_WIFI_NONE},         {PROTO_WIFI_STATE_CONNECTING, UI_WIFI_NONE},
        {PROTO_WIFI_STATE_CONNECTED, UI_WIFI_OK},      {PROTO_WIFI_STATE_AUTH_FAILED, UI_WIFI_NONE},
        {PROTO_WIFI_STATE_NOT_FOUND, UI_WIFI_NONE},    {PROTO_WIFI_STATE_NO_INTERNET, UI_WIFI_NONE},
        {PROTO_WIFI_STATE_UNCONFIGURED, UI_WIFI_NONE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        st.wifi_state = cases[i].state;
        ui_set_status(&st);
        run(2100);
        TEST_ASSERT_EQUAL_INT(cases[i].icon, ui_wifi_icon());
        /* the fan takes room in the footer, left of the Bluetooth rune */
        TEST_ASSERT_TRUE((black_in(100, 176, 200, 200) > bare) == (cases[i].icon != UI_WIFI_NONE));
    }
    st.wifi_state = PROTO_WIFI_STATE_CONNECTED;
    ui_set_status(&st);
    ui_set_power(false);
    run(2100);
    TEST_ASSERT_EQUAL_INT(UI_WIFI_NONE, ui_wifi_icon()); /* powered off */
}

static void test_setting_notice(void)
{
    ui_set_time(T_14_27_05);
    ui_status_t st = {.battery_pct = 57, .connected = true, .ota_pct = -1, .wifi_state = PROTO_WIFI_STATE_IDLE};
    ui_set_status(&st);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "57%"));
    TEST_ASSERT_EQUAL_INT(UI_NOTICE_NONE, ui_current_notice());
    const uint32_t footer = black_in(0, 176, 200, 200);
    ui_show_notice(UI_NOTICE_SETTING);
    run(500);
    TEST_ASSERT_EQUAL_INT(UI_NOTICE_SETTING, ui_current_notice());
    TEST_ASSERT_NOT_NULL(strstr(text(), "Setting updated\n"));
    TEST_ASSERT_NULL(strstr(text(), "57%")); /* it takes the footer */
    TEST_ASSERT_TRUE(black_in(0, 176, 200, 200) > 3 * footer); /* a black bar */
    /* a newer message replaces it and restarts the time */
    run(2000);
    ui_show_notice(UI_NOTICE_WIFI);
    run(2000);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Wi-Fi saved\n"));
    TEST_ASSERT_NULL(strstr(text(), "Setting updated"));
    /* it goes after UI_NOTICE_MS and the footer is back */
    run(1500);
    TEST_ASSERT_EQUAL_INT(UI_NOTICE_NONE, ui_current_notice());
    TEST_ASSERT_NULL(strstr(text(), "Wi-Fi saved"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "57%"));
    TEST_ASSERT_EQUAL_UINT32(footer, black_in(0, 176, 200, 200));
    /* in Vietnamese, over a card, and not while powered off */
    ui_config_t c = cfg(10);
    c.language = PROTO_LANGUAGE_VI;
    ui_set_config(&c);
    npipe_notif_t n = notif(1, "Zalo", "Hi", "");
    ui_on_notification(&n);
    ui_show_notice(UI_NOTICE_SETTING);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Đã cập nhật cài đặt\n"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Hi\n")); /* the card stays */
    ui_set_power(false);
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Đã cập nhật"));
}

static void test_cards_time_out_to_the_clock(void)
{
    ui_params_t p = {.display = board_get()->display, .blob = &blob.iface, .now_ms = now_ms,
                     .cards_timeout_s = UI_CARDS_TIMEOUT_S};
    ui_deinit();
    TEST_ASSERT_TRUE(ui_init(&p));
    ui_config_t c = cfg(10);
    ui_set_config(&c);
    ui_set_time(T_14_27_05);
    npipe_notif_t a = notif(1, "Zalo", "First", "a"), b = notif(2, "Mail", "Second", "b");
    ui_on_notification(&a);
    ui_on_notification(&b);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail\n1/2"));
    run(50000); /* 52 s in */
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT); /* a key restarts the minute */
    run(2000);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo\n2/2"));
    run(50000); /* 52 s after the key */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo\n2/2"));
    run(10000); /* more than a minute */
    TEST_ASSERT_NULL(strstr(text(), "First"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "\n2\n")); /* the clock, the badge counts the two waiting */
    /* the notifications stayed: short PWR opens them at the newest card, a new one opens its card */
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail\n1/2"));
    run(61000);
    TEST_ASSERT_NULL(strstr(text(), "Second"));
    npipe_notif_t n = notif(3, "Chat", "Third", "c");
    ui_on_notification(&n);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Chat\n1/3"));
    /* a board that sets no timeout keeps the card until it is stepped past */
    p.cards_timeout_s = 0;
    ui_deinit();
    TEST_ASSERT_TRUE(ui_init(&p));
    ui_set_config(&c);
    ui_on_notification(&a);
    run(2100);
    run(120000);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo\n1/1"));
}

/* A card stays on the panel for UI_CARD_MIN_MS before a newer notification replaces it (PLAN D81). */
static void test_card_stays_before_the_next_one_shows(void)
{
    run(2100); /* the boot refresh is over */
    npipe_notif_t a = notif(1, "Zalo", "First", "a"), b = notif(2, "Mail", "Second", "b"), c = notif(3, "Chat", "Third", "c");
    ui_on_notification(&a);
    run(500); /* on the panel since about 300 ms */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo\n1/1"));
    ui_on_notification(&b); /* arrives 200 ms after the first one was shown */
    ui_on_notification(&c);
    run(1000);
    /* the first card keeps the screen; its counter says two newer ones are waiting */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo\n3/3"));
    TEST_ASSERT_NULL(strstr(text(), "Third"));
    run(1000); /* its two seconds are over: the next one in arrival order opens, not the newest */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail\n2/3"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Second"));
    run(1500); /* and stays its own two seconds */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail\n2/3"));
    run(1000);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Chat\n1/3"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Third"));
    TEST_ASSERT_EQUAL_UINT32(0, state().violations);
    /* once it has been there long enough, the next one shows at once */
    run(2200);
    npipe_notif_t d = notif(4, "Gmail", "Fourth", "d");
    ui_on_notification(&d);
    run(50);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Gmail\n1/4"));
    /* the card on screen posted again is the same card: no wait */
    run(500);
    d.kind = NPIPE_ADDED;
    snprintf(d.body, sizeof(d.body), "again");
    d.body_len = 5;
    ui_on_notification(&d);
    run(50);
    TEST_ASSERT_NOT_NULL(strstr(text(), "again"));
    /* a key that moves to another card starts that card's time too */
    run(2200);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT); /* Chat, now on the panel */
    run(500);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Chat\n2/4"));
    npipe_notif_t e = notif(5, "Slack", "Fifth", "e");
    ui_on_notification(&e);
    run(200);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Chat\n3/5")); /* held */
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Slack\n1/5"));
    /* the card that is held goes away before its time is up: the next one opens at once */
    npipe_notif_t f = notif(6, "Zing", "Sixth", "f");
    ui_on_notification(&f);
    run(100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Slack\n2/6"));
    f.kind = NPIPE_REMOVED;
    npipe_notif_t held = e;
    held.kind = NPIPE_REMOVED;
    ui_on_notification(&held);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zing\n1/5"));
    /* one that arrives while another waits goes behind it: g opens at once (Zing has been there long enough),
     * h waits for g's two seconds */
    run(2500); /* (that refresh was a full one: its two seconds count from its end) */
    npipe_notif_t g = notif(7, "Meet", "Seventh", "g"), h = notif(8, "Cal", "Eighth", "h");
    ui_on_notification(&g);
    run(100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Meet\n1/6"));
    ui_on_notification(&h);
    run(100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Meet\n2/7"));
    TEST_ASSERT_NULL(strstr(text(), "Eighth"));
    run(2500);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Cal\n1/7"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Eighth"));
}

static void test_keys_step_back_to_clock(void)
{
    npipe_notif_t a = notif(1, "Zalo", "First", "a"), b = notif(2, "Mail", "Second", "b");
    ui_on_notification(&a);
    ui_on_notification(&b); /* newest first: Mail is 1/2 */
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail\n1/2"));
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT); /* newer than the newest: the clock */
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Second"));
    TEST_ASSERT_NULL(strstr(text(), "1/2"));
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT); /* from the clock: the newest card */
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail\n1/2"));
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo\n2/2"));
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT); /* older than the oldest: the clock, no wrap-around */
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "First"));
    TEST_ASSERT_NULL(strstr(text(), "2/2"));
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT); /* from the clock short BOOT opens the settings (D98) */
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n1/9\n"));
    TEST_ASSERT_NULL(strstr(text(), "First"));
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_LONG); /* back to the clock; a new notification opens its card */
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
    npipe_notif_t c = notif(3, "Chat", "Third", "c");
    ui_on_notification(&c);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Chat\n1/3"));
    /* a single card: any key leaves it, any key brings it back */
    b.kind = NPIPE_REMOVED;
    a.kind = NPIPE_REMOVED;
    ui_on_notification(&a);
    ui_on_notification(&b);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Chat\n1/1"));
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT);
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Third"));
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT); /* short PWR on the clock brings the card back */
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Chat\n1/1"));
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT); /* short PWR leaves a single card too, to the clock */
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Third"));
}

static void test_cards_and_buttons(void)
{
    npipe_notif_t a = notif(1, "Zalo", "First", "a"), b = notif(2, "Mail", "Second", "b");
    ui_on_notification(&a);
    ui_on_notification(&b);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail\n1/2\nSecond\nb\n"));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT));
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo\n2/2\nFirst\n"));
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "1/2"));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_PAIRING_TOGGLE, ui_on_button(BOARD_BTN_BOOT_PWR, BOARD_PRESS_LONG));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT_PWR, BOARD_PRESS_SHORT));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_LONG)); /* 1.5 s: not the power off */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_POWER_TOGGLE, ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_HOLD)); /* 5 s is (D96) */
    /* removal */
    b.kind = NPIPE_REMOVED;
    ui_on_notification(&b);
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo\n1/1\nFirst\n"));
    a.kind = NPIPE_REMOVED;
    ui_on_notification(&a);
    ui_on_notification(&a); /* unknown id: ignored */
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Time not set"));
}

static void test_keeps_the_newest_cards(void)
{
    for (uint32_t i = 1; i <= UI_MAX_CARDS + 3; i++) {
        npipe_notif_t n = notif(i, "App", "T", "");
        ui_on_notification(&n);
    }
    run(2100);
    char want[16];
    snprintf(want, sizeof(want), "1/%u", UI_MAX_CARDS);
    TEST_ASSERT_NOT_NULL(strstr(text(), want));
}

static void test_status_language_and_privacy(void)
{
    npipe_notif_t n = notif(1, "Zalo", "Tiêu đề", "Nội dung riêng tư");
    n.timestamp = 1791700215; /* 06:30:15 UTC */
    ui_on_notification(&n);
    ui_config_t c = {.language = PROTO_LANGUAGE_VI, .show_body = false, .full_refresh_every = 10,
                     .tz_offset_min = 420, .clock_24h = true};
    ui_set_config(&c);
    ui_set_time(1791700215 + 125); /* the footer shows the current time, not the notification's */
    ui_status_t st = {.battery_pct = 57, .connected = true, .ota_pct = -1};
    ui_set_status(&st);
    run(2100);
    TEST_ASSERT_EQUAL_STRING("Zalo\n1/1\nTiêu đề\n\n13:32\n57%\n", text());
    TEST_ASSERT_EQUAL_INT(UI_LINK_CONNECTED, ui_link_icon());
    st.pairing_mode = true;
    ui_set_status(&st);
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Chế độ ghép nối"));
    st.pairing_mode = false;
    st.ota_pct = 45;
    st.external_power = true;
    ui_set_status(&st);
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Đang cập nhật 45%\n57%"));
    n.kind = NPIPE_REMOVED;
    ui_on_notification(&n);
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "13:32\nChủ nhật, 11/10\n"));
}

/* display.mask_numbers: numbers of 4 or more digits are drawn as ****, shorter ones stay. */
static void mask(const char *in, const char *want)
{
    char out[64];
    ui_mask_numbers(out, sizeof(out), in);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

static void test_mask_numbers(void)
{
    mask("123", "123");
    mask("1234", "****");
    mask("1.234", "****");
    mask("12345", "****");
    mask("1,234,567.89 VND", "**** VND");
    mask("Code 482913 expires in 5 min", "Code **** expires in 5 min");
    mask("12.5 and 3,5", "12.5 and 3,5");
    mask("12:34 on 5.", "12:34 on 5.");           /* a colon or a trailing dot ends the number */
    mask("1234,", "****,");
    mask("a1234b56c7890", "a****b56c****");
    mask("Số dư 1.250.000đ", "Số dư ****đ");
    mask("", "");
    mask("999", "999");
    mask("1.2.3.4", "****");
    /* in place, and cut to the buffer */
    char s[] = "pin 1234 and 56";
    ui_mask_numbers(s, sizeof(s), s);
    TEST_ASSERT_EQUAL_STRING("pin **** and 56", s);
    char small[7];
    ui_mask_numbers(small, sizeof(small), "ab 123456 cd");
    TEST_ASSERT_EQUAL_STRING("ab ***", small);
    ui_mask_numbers(small, 0, "x");                /* no room: untouched, no write */
}

static void test_card_masks_numbers_when_asked(void)
{
    npipe_notif_t n = notif(1, "Bank", "Balance 1.234.567", "Your code is 482913. Call 112");
    ui_on_notification(&n);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Balance 1.234.567"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "482913"));
    ui_config_t c = cfg(10);
    c.mask_numbers = true;
    ui_set_config(&c);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Balance ****"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Your code is ****. Call 112"));
    TEST_ASSERT_NULL(strstr(text(), "482913"));
    TEST_ASSERT_NULL(strstr(text(), "1.234"));
    c.mask_numbers = false;                         /* the notification itself was not changed */
    ui_set_config(&c);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "482913"));
}

/* The passkey takes the card's place on the Paper and leaves with the pairing. */
static void test_passkey_screen(void)
{
    npipe_notif_t n = notif(1, "Zalo", "Hello", "Body");
    ui_on_notification(&n);
    ui_status_t st = {.battery_pct = 57, .pairing_mode = true, .has_passkey = true, .passkey = 123456, .ota_pct = -1};
    ui_set_status(&st);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Pairing code\nEnter this code on your phone\n123456\n"));
    TEST_ASSERT_NULL(strstr(text(), "Hello"));
    st.passkey = 7;
    ui_set_status(&st);
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "\n000007\n"));
    st.has_passkey = false;
    st.pairing_mode = false;
    ui_set_status(&st);
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Hello"));
    TEST_ASSERT_NULL(strstr(text(), "000007"));
}

static void test_power_off_screen(void)
{
    run(2100);
    ui_set_power(false);
    run(400);
    TEST_ASSERT_EQUAL_STRING("ePager\nPowered off\n", text());
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_LONG));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_POWER_TOGGLE, ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_HOLD)); /* the same key powers on */
    TEST_ASSERT_FALSE(pixel_black(0, 0)); /* the bar is gone */
}

/* ---------------------------------------------------------------- settings list (PLAN D96) */

static ui_action_t seen_action;
static uint32_t seen_arg;
static int seen_n;
static void on_action(void *ctx, ui_action_t a, uint32_t arg)
{
    (void)ctx;
    seen_action = a;
    seen_arg = arg;
    seen_n++;
}

static void start_settings(bool wifi)
{
    ui_params_t p = {.display = board_get()->display, .blob = &blob.iface, .now_ms = now_ms, .wifi_setup = wifi,
                     .on_action = on_action, .cards_timeout_s = UI_CARDS_TIMEOUT_S};
    ui_deinit();
    TEST_ASSERT_TRUE(ui_init(&p));
    ui_config_t c = cfg(10);
    c.weather_enabled = true;
    ui_set_config(&c);
    ui_set_time(T_14_27_05);
    seen_action = UI_ACTION_NONE;
    seen_arg = seen_n = 0;
    run(2100);
}

/* The highlighted row is a black bar from x = 2: its left edge is black, the other rows' are white. */
static bool row_selected(unsigned row_on_screen) { return pixel_black(3, 25 + 25 * row_on_screen + 12); }

static void test_settings_navigation(void)
{
    start_settings(true);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT)); /* the clock: open the list */
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n1/12\nClock\n24h\nTemperature\n°C\nLanguage\nEnglish\n"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Message text\nOn\nHide numbers\nOff\nRotation\n0°\nWeather\nOn\n"));
    TEST_ASSERT_NULL(strstr(text(), "Hold")); /* no key hints: the rows fill the panel */
    TEST_ASSERT_TRUE(row_selected(0));
    TEST_ASSERT_FALSE(row_selected(1));
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT); /* PWR = down */
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n2/12\n"));
    TEST_ASSERT_TRUE(row_selected(1));
    TEST_ASSERT_FALSE(row_selected(0));
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT); /* BOOT = up */
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT); /* up from the first row wraps to the last, the list scrolls */
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n12/12\n"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "About\n>\n")); /* About is the last row */
    TEST_ASSERT_NULL(strstr(text(), "Clock\n"));
    TEST_ASSERT_TRUE(row_selected(6)); /* the last row is at the bottom of the window */
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT); /* down from the last wraps to the first */
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n1/12\nClock\n"));
    TEST_ASSERT_TRUE(row_selected(0));
    TEST_ASSERT_EQUAL_INT(0, seen_n); /* moving selects nothing */
    /* back: long PWR; long BOOT outside the list does nothing */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_LONG));
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "14:27")); /* the clock */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG));
    TEST_ASSERT_EQUAL_INT(0, seen_n);
}

static void test_settings_select_changes_values(void)
{
    start_settings(true);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT);
    /* a choice reports its config key and the next value; the firmware stores it and hands the configuration back */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_SETTING, seen_action);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_UI_CLOCK_24H << 8 | 0u, seen_arg);
    ui_config_t c = cfg(10);
    c.clock_24h = false;
    c.weather_enabled = true;
    ui_set_config(&c);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Clock\n12h\n"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n1/12\n")); /* still on the same row */
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_UI_TEMP_UNIT << 8 | 1u, seen_arg); /* Celsius -> Fahrenheit */
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_UI_LANGUAGE << 8 | 1u, seen_arg);
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_DISPLAY_SHOW_BODY << 8 | 0u, seen_arg); /* on -> off */
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_DISPLAY_MASK_NUMBERS << 8 | 1u, seen_arg);
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_DISPLAY_ORIENTATION << 8 | 1u, seen_arg); /* 0 -> 90 */
    c.orientation = 3;
    ui_set_config(&c);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_DISPLAY_ORIENTATION << 8 | 0u, seen_arg); /* 270 -> 0: it wraps */
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_WEATHER_ENABLED << 8 | 0u, seen_arg); /* weather on -> off */
    TEST_ASSERT_EQUAL_INT(8, seen_n);
}

static void test_settings_actions(void)
{
    start_settings(true);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT);
    for (int i = 0; i < 7; i++) {
        ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    }
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n8/12\n"));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_PAIRING_TOGGLE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG));
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Settings")); /* pairing is a screen of its own: the list is closed */
    /* the Wi-Fi setup opens over the list and any key leads back to it */
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT);
    for (int i = 0; i < 8; i++) {
        ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    }
    TEST_ASSERT_EQUAL_INT(UI_ACTION_WIFI_SETUP, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG));
    ui_wifi_setup_t st = {.state = UI_SETUP_WAITING};
    ui_set_wifi_setup(&st);
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_WIFI_SETUP_CLOSE, ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_LONG));
    st.state = UI_SETUP_OFF;
    ui_set_wifi_setup(&st);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n9/12\n"));
    /* pairing mode that opens by itself (the last phone was forgotten) closes the list */
    ui_status_t status = {.battery_pct = 80, .pairing_mode = true, .ota_pct = -1};
    ui_set_status(&status);
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
    /* a board without Wi-Fi has neither the weather, the Wi-Fi row nor the networks page */
    start_settings(false);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n1/9\n"));
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n9/9\n"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Pairing mode\n"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Paired phones\n"));
    TEST_ASSERT_NULL(strstr(text(), "Weather"));
    TEST_ASSERT_NULL(strstr(text(), "Wi-Fi"));
}

/* The pages (PLAN D97): paired phones and saved networks (select, "Forget?", select again), then About. */
static void test_settings_pages(void)
{
    start_settings(true);
    ui_info_t info;
    memset(&info, 0, sizeof(info));
    info.n_phones = 2;
    info.phone_connected[1] = true;
    info.n_networks = 2;
    snprintf(info.networks[0], sizeof(info.networks[0]), "%s", "Home");
    snprintf(info.networks[1], sizeof(info.networks[1]), "%s", "Office");
    snprintf(info.firmware, sizeof(info.firmware), "%s", "1.2.3");
    snprintf(info.device, sizeof(info.device), "%s", "ePager-1A2B");
    snprintf(info.board, sizeof(info.board), "%s", "waveshare_s3_epaper_154");
    ui_set_info(&info);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT);
    for (int i = 0; i < 9; i++) {
        ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    }
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n10/12\n"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Paired phones\n>\n"));
    seen_n = 0;
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG)); /* open the page */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_INFO_REFRESH, seen_action); /* the firmware is asked for the lists */
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Paired phones\n1/2\nPhone 1\n\nPhone 2\nconnected\n"));
    TEST_ASSERT_TRUE(row_selected(0));
    seen_n = 0;
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG); /* once: asks */
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Phone 1\nForget?\n"));
    TEST_ASSERT_EQUAL_INT(0, seen_n);
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT); /* moving away cancels */
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT);
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Forget?"));
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT); /* the second phone */
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG); /* twice: forgets it */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_FORGET_PHONE, seen_action);
    TEST_ASSERT_EQUAL_UINT32(1, seen_arg);
    info.n_phones = 1; /* the firmware answers; the highlight stays on a row that exists */
    info.phone_connected[1] = false;
    ui_set_info(&info);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Paired phones\n1/1\nPhone 1\n"));
    TEST_ASSERT_NULL(strstr(text(), "Phone 2"));
    /* back to the list, on the row that opened the page */
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_LONG);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n10/12\n"));
    /* the networks */
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Wi-Fi networks\n1/2\nHome\n\nOffice\n\n"));
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Office\nForget?\n"));
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_FORGET_NETWORK, seen_action);
    TEST_ASSERT_EQUAL_UINT32(1, seen_arg);
    info.n_networks = 0; /* nothing left: one row says so and selecting it does nothing */
    ui_set_info(&info);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Wi-Fi networks\n1/1\nNo networks\n"));
    seen_n = 0;
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    TEST_ASSERT_EQUAL_INT(0, seen_n);
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_LONG);
    /* About, with the battery from the status */
    ui_status_t status = {.battery_pct = 85, .ota_pct = -1};
    ui_set_status(&status);
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "About\n1/5\nFirmware\n1.2.3\nDevice\nePager-1A2B\nBoard\ns3_epaper_154\n"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Battery\n85%\nProtocol\n" PROTO_CONTRACT_VERSION "\n"));
    seen_n = 0;
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_LONG); /* read-only */
    TEST_ASSERT_EQUAL_INT(0, seen_n);
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_LONG);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n12/12\n"));
    TEST_ASSERT_EQUAL_UINT32(0, state().violations);
}

static void test_settings_power_off_timeout_and_notifications(void)
{
    start_settings(true);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT);
    run(2100);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_POWER_TOGGLE, ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_HOLD)); /* from the list too */
    /* a minute without a key: back to the clock; a key in between starts the minute again */
    run(50000);
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT); /* down a row */
    run(50000);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n2/12\n"));
    run(11000);
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "14:27"));
    /* the next visit starts at the first row again */
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n1/12\n"));
    /* a notification replaces the list */
    npipe_notif_t a = notif(1, "Zalo", "First", "a");
    ui_on_notification(&a);
    run(2100);
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo\n1/1"));
    /* short PWR on a card steps to the clock, short BOOT opens the list */
    ui_on_button(BOARD_BTN_PWR, BOARD_PRESS_SHORT);
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n1/12\n"));
    /* the page in Vietnamese, with the language row naming both languages */
    ui_config_t c = cfg(10);
    c.language = PROTO_LANGUAGE_VI;
    c.weather_enabled = true;
    ui_set_config(&c);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Cài đặt\n1/12\nĐồng hồ\n24h\nNhiệt độ\n°C\nNgôn ngữ\nTiếng Việt\n"));
    TEST_ASSERT_EQUAL_UINT32(0, state().violations);
}

/* ---------------------------------------------------------------- icons (protocol.md 6.4) */

static npipe_notif_t from_app(uint32_t id, const char *app_id, uint8_t category)
{
    npipe_notif_t n = notif(id, "App", "Title", "Body");
    snprintf(n.app_id, sizeof(n.app_id), "%s", app_id);
    n.app_id_len = (uint16_t)strlen(app_id);
    n.category = category;
    return n;
}

/* The 24x24 region of the card icon equals `bits` (mono1: bit 1 = white). */
static bool icon_region_is(const uint8_t *bits)
{
    for (uint32_t y = 0; y < UI_ICON_SIZE; y++) {
        for (uint32_t x = 0; x < UI_ICON_SIZE; x++) {
            bool black = !((bits[y * 3 + x / 8] >> (7 - x % 8)) & 1);
            if (pixel_black(ICON_X + x, ICON_Y + y) != black) {
                return false;
            }
        }
    }
    return true;
}

/* An installed app_icon file for app_id: a checkerboard of 3x3 cells, XOR-ed with `seed`. */
static void checker(uint8_t bits[UI_ICON_BYTES], uint8_t seed)
{
    memset(bits, 0, UI_ICON_BYTES);
    for (uint32_t y = 0; y < UI_ICON_SIZE; y++) {
        for (uint32_t x = 0; x < UI_ICON_SIZE; x++) {
            if (((x / 3 + y / 3 + seed) & 1) == 0) {
                bits[y * 3 + x / 8] |= (uint8_t)(0x80 >> (x % 8)); /* white */
            }
        }
    }
}

static void install_icon_file(const char *app_id, const uint8_t bits[UI_ICON_BYTES], uint8_t type, uint8_t fmt,
                              uint16_t w, uint16_t h)
{
    uint8_t file[16 + UI_ICON_BYTES] = {'E', 'S', 'P', 'A', 1, type, fmt, 0};
    file[8] = (uint8_t)w;
    file[10] = (uint8_t)h;
    file[12] = UI_ICON_BYTES;
    memcpy(file + 16, bits, UI_ICON_BYTES);
    char id[XFER_APP_ICON_ID_LEN + 1], name[BLOB_NAME_MAX + 1];
    xfer_app_icon_id(app_id, strlen(app_id), id);
    xfer_asset_blob_name(PROTO_ASSET_TYPE_APP_ICON, id, XFER_APP_ICON_ID_LEN, name);
    TEST_ASSERT_EQUAL_INT(BLOB_OK, blob.iface.write(blob.iface.ctx, name, 0, file, sizeof(file)));
}

static void install_icon(const char *app_id, const uint8_t bits[UI_ICON_BYTES])
{
    install_icon_file(app_id, bits, PROTO_ASSET_TYPE_APP_ICON, PROTO_ASSET_FORMAT_MONO1, 24, 24);
}

static void test_builtin_icon_per_category(void)
{
    TEST_ASSERT_EQUAL_INT(UI_ICON_NONE, ui_current_icon());
    for (uint8_t cat = 0; cat < UI_BUILTIN_ICON_COUNT; cat++) {
        npipe_notif_t n = from_app(cat + 1u, "com.example.nobody", cat);
        ui_on_notification(&n);
        run(4200);
        TEST_ASSERT_EQUAL_INT_MESSAGE(UI_ICON_BUILTIN, ui_current_icon(), "category icon");
        TEST_ASSERT_TRUE_MESSAGE(icon_region_is(ui_builtin_icons[cat]), "icon pixels");
    }
    /* a value outside the enum (the pipeline maps it, the UI must not read past the table) uses other's */
    npipe_notif_t n = from_app(99, "com.example.nobody", 200);
    ui_on_notification(&n);
    run(2100);
    TEST_ASSERT_TRUE(icon_region_is(ui_builtin_icons[PROTO_CATEGORY_OTHER]));
    TEST_ASSERT_EQUAL_UINT32(0, state().violations);
}

static void test_no_icon_without_a_card(void)
{
    run(2100);
    TEST_ASSERT_EQUAL_INT(UI_ICON_NONE, ui_current_icon());
    /* the idle title starts at the margin, so the icon area is not blank: compare with the icons */
    for (size_t c = 0; c < UI_BUILTIN_ICON_COUNT; c++) {
        TEST_ASSERT_FALSE(icon_region_is(ui_builtin_icons[c]));
    }
    npipe_notif_t n = from_app(1, "a", PROTO_CATEGORY_EMAIL);
    ui_on_notification(&n);
    run(400);
    n.kind = NPIPE_REMOVED;
    ui_on_notification(&n);
    run(400);
    TEST_ASSERT_EQUAL_INT(UI_ICON_NONE, ui_current_icon());
    ui_set_power(false);
    run(400);
    TEST_ASSERT_EQUAL_INT(UI_ICON_NONE, ui_current_icon());
}

static void test_app_icon_replaces_the_category_icon(void)
{
    uint8_t bits[UI_ICON_BYTES];
    checker(bits, 0);
    install_icon("com.zing.zalo", bits);
    npipe_notif_t n = from_app(1, "com.zing.zalo", PROTO_CATEGORY_SOCIAL);
    ui_on_notification(&n);
    run(2100);
    TEST_ASSERT_EQUAL_INT(UI_ICON_APP, ui_current_icon());
    TEST_ASSERT_TRUE(icon_region_is(bits));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Title")); /* the title still shows next to it */
    /* another app (and the same ID in another case: ids are case-sensitive) keeps the category icon */
    npipe_notif_t o = from_app(2, "com.zing.ZALO", PROTO_CATEGORY_SOCIAL);
    ui_on_notification(&o);
    run(4200);
    TEST_ASSERT_EQUAL_INT(UI_ICON_BUILTIN, ui_current_icon());
    TEST_ASSERT_TRUE(icon_region_is(ui_builtin_icons[PROTO_CATEGORY_SOCIAL]));
    ui_on_button(BOARD_BTN_BOOT, BOARD_PRESS_SHORT); /* back to the older card */
    run(400);
    TEST_ASSERT_EQUAL_INT(UI_ICON_APP, ui_current_icon());
    TEST_ASSERT_TRUE(icon_region_is(bits));
}

static void test_icon_installed_or_replaced_later_applies_at_the_next_render(void)
{
    uint8_t one[UI_ICON_BYTES], two[UI_ICON_BYTES];
    checker(one, 0);
    checker(two, 1);
    npipe_notif_t n = from_app(1, "com.zing.zalo", PROTO_CATEGORY_SOCIAL);
    ui_on_notification(&n);
    run(2100);
    TEST_ASSERT_EQUAL_INT(UI_ICON_BUILTIN, ui_current_icon());
    uint32_t partial = state().partial;

    ui_assets_changed(PROTO_ASSET_TYPE_BACKGROUND); /* not an icon: nothing to redraw */
    run(400);
    TEST_ASSERT_EQUAL_UINT32(partial, state().partial);

    install_icon("com.zing.zalo", one);
    ui_assets_changed(PROTO_ASSET_TYPE_APP_ICON);
    run(400);
    TEST_ASSERT_EQUAL_INT(UI_ICON_APP, ui_current_icon());
    TEST_ASSERT_TRUE(icon_region_is(one));
    TEST_ASSERT_EQUAL_UINT32(partial + 1, state().partial);

    install_icon("com.zing.zalo", two); /* replaced under the same id */
    ui_assets_changed(PROTO_ASSET_TYPE_APP_ICON);
    run(400);
    TEST_ASSERT_TRUE(icon_region_is(two));

    char id[XFER_APP_ICON_ID_LEN + 1], name[BLOB_NAME_MAX + 1]; /* deleted: back to the category icon */
    xfer_app_icon_id("com.zing.zalo", 13, id);
    xfer_asset_blob_name(PROTO_ASSET_TYPE_APP_ICON, id, XFER_APP_ICON_ID_LEN, name);
    TEST_ASSERT_EQUAL_INT(BLOB_OK, blob.iface.remove(blob.iface.ctx, name));
    ui_assets_changed(PROTO_ASSET_TYPE_APP_ICON);
    run(400);
    TEST_ASSERT_EQUAL_INT(UI_ICON_BUILTIN, ui_current_icon());
    TEST_ASSERT_TRUE(icon_region_is(ui_builtin_icons[PROTO_CATEGORY_SOCIAL]));
    TEST_ASSERT_EQUAL_UINT32(0, state().violations);
}

static void test_malformed_icon_files_are_ignored(void)
{
    uint8_t bits[UI_ICON_BYTES];
    checker(bits, 0);
    npipe_notif_t n = from_app(1, "com.example.bad", PROTO_CATEGORY_NEWS);
    ui_on_notification(&n);
    run(2100);
    /* wrong type, wrong format, wrong size, wrong payload length: all fall back to the category icon */
    install_icon_file(n.app_id, bits, PROTO_ASSET_TYPE_BACKGROUND, PROTO_ASSET_FORMAT_MONO1, 24, 24);
    ui_assets_changed(PROTO_ASSET_TYPE_APP_ICON);
    run(400);
    TEST_ASSERT_EQUAL_INT(UI_ICON_BUILTIN, ui_current_icon());
    install_icon_file(n.app_id, bits, PROTO_ASSET_TYPE_APP_ICON, PROTO_ASSET_FORMAT_FONT_V1, 24, 24);
    ui_assets_changed(PROTO_ASSET_TYPE_APP_ICON);
    run(400);
    TEST_ASSERT_EQUAL_INT(UI_ICON_BUILTIN, ui_current_icon());
    install_icon_file(n.app_id, bits, PROTO_ASSET_TYPE_APP_ICON, PROTO_ASSET_FORMAT_MONO1, 32, 32);
    ui_assets_changed(PROTO_ASSET_TYPE_APP_ICON);
    run(400);
    TEST_ASSERT_EQUAL_INT(UI_ICON_BUILTIN, ui_current_icon());
    /* a file of the wrong length (stored by a bug or corruption) */
    char id[XFER_APP_ICON_ID_LEN + 1], name[BLOB_NAME_MAX + 1];
    xfer_app_icon_id(n.app_id, strlen(n.app_id), id);
    xfer_asset_blob_name(PROTO_ASSET_TYPE_APP_ICON, id, XFER_APP_ICON_ID_LEN, name);
    TEST_ASSERT_EQUAL_INT(BLOB_OK, blob.iface.remove(blob.iface.ctx, name));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, blob.iface.write(blob.iface.ctx, name, 0, "ESPA", 4));
    ui_assets_changed(PROTO_ASSET_TYPE_APP_ICON);
    run(400);
    TEST_ASSERT_EQUAL_INT(UI_ICON_BUILTIN, ui_current_icon());
    TEST_ASSERT_TRUE(icon_region_is(ui_builtin_icons[PROTO_CATEGORY_NEWS]));
}

static void test_icon_without_blob_storage(void)
{
    ui_deinit();
    sim_epaper_reset();
    ui_params_t p = {.display = board_get()->display, .now_ms = now_ms}; /* no blob: built-in icons only */
    TEST_ASSERT_TRUE(ui_init(&p));
    npipe_notif_t n = from_app(1, "com.zing.zalo", PROTO_CATEGORY_SCHEDULE);
    ui_on_notification(&n);
    run(2100);
    TEST_ASSERT_EQUAL_INT(UI_ICON_BUILTIN, ui_current_icon());
    TEST_ASSERT_TRUE(icon_region_is(ui_builtin_icons[PROTO_CATEGORY_SCHEDULE]));
}

static void test_long_title_stays_on_one_line_beside_the_icon(void)
{
    npipe_notif_t n = from_app(1, "com.zing.zalo", PROTO_CATEGORY_SOCIAL);
    snprintf(n.title, sizeof(n.title), "%s", "A fairly long title that would need a second line to fit");
    n.title_len = (uint16_t)strlen(n.title);
    n.body[0] = '\0';
    n.body_len = 0;
    ui_on_notification(&n);
    run(2100);
    TEST_ASSERT_TRUE(icon_region_is(ui_builtin_icons[PROTO_CATEGORY_SOCIAL]));
    TEST_ASSERT_NOT_NULL(strstr(text(), "A fairly long"));
    bool line_1 = false;
    for (uint32_t y = ICON_Y; y < ICON_Y + UI_ICON_SIZE; y++) {
        for (uint32_t x = 32; x < 190; x++) {
            line_1 = line_1 || pixel_black(x, y);
        }
    }
    TEST_ASSERT_TRUE(line_1);
    /* no second line: with an empty body, nothing below the icon (the body starts at ICON_Y + 24 + GAP) */
    for (uint32_t y = ICON_Y + UI_ICON_SIZE; y < ICON_Y + UI_ICON_SIZE + 20; y++) {
        for (uint32_t x = 0; x < 200; x++) {
            TEST_ASSERT_FALSE(pixel_black(x, y));
        }
    }
}

/* ---------------------------------------------------------------- idle screen: clock and climate */


static void test_idle_clock_redraws_once_a_minute(void)
{
    ui_set_time(T_14_27_05);
    run(2100);
    TEST_ASSERT_EQUAL_STRING("14:27\nSunday, 11 Oct\n\n\n", text());
    ui_stats_t a, b;
    ui_get_stats(&a);
    for (uint32_t s = 1; s < 55; s++) { /* same minute: nothing rendered or refreshed */
        ui_set_time(T_14_27_05 + s);
        run(10);
    }
    ui_get_stats(&b);
    TEST_ASSERT_EQUAL_UINT32(a.partial + a.full, b.partial + b.full);
    TEST_ASSERT_EQUAL_UINT32(a.skipped, b.skipped);
    ui_set_time(T_14_27_05 + 55); /* 14:28:00 */
    run(400);
    ui_get_stats(&b);
    TEST_ASSERT_EQUAL_UINT32(a.partial + 1, b.partial);
    TEST_ASSERT_NOT_NULL(strstr(text(), "14:28\n"));
    ui_set_time(0); /* clock lost */
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "--:--\nTime not set\n"));
}

static void test_clock_time_zone_and_date_rollover(void)
{
    ui_config_t c = cfg(10);
    c.tz_offset_min = 600; /* UTC+10: 00:27 on Monday */
    ui_set_config(&c);
    ui_set_time(T_14_27_05);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "00:27\nMonday, 12 Oct\n"));
    c.tz_offset_min = -720;
    ui_set_config(&c);
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "02:27\nSunday, 11 Oct\n"));
}

static void test_clock_12_hour(void)
{
    ui_config_t c = cfg(10);
    c.clock_24h = false;
    ui_set_config(&c);
    ui_set_time(T_14_27_05);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "2:27\nPM\nSunday"));
    ui_set_time(T_14_27_05 - 14 * 3600 - 22 * 60); /* 00:05 */
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "12:05\nAM\n"));
    ui_set_time(T_14_27_05 - 2 * 3600 - 22 * 60); /* 12:05 */
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "12:05\nPM\n"));
    npipe_notif_t n = notif(1, "Zalo", "Hi", "");
    ui_on_notification(&n);
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "\n12:05 PM\n")); /* the card footer */
    TEST_ASSERT_NULL(strstr(text(), "\nPM\n"));          /* no idle clock behind the card */
}

/* Columns just inside a box's left and right borders (straight part, below the rounded corners) stay
 * white over rows y0..y1: the text fits the box. */
static void assert_inside(uint32_t box_x, uint32_t box_w, uint32_t y0, uint32_t y1)
{
    for (uint32_t y = y0; y <= y1; y++) {
        for (uint32_t d = 2; d < 4; d++) {
            TEST_ASSERT_FALSE_MESSAGE(pixel_black(box_x + d, y) || pixel_black(box_x + box_w - 1 - d, y), text());
        }
    }
}

/* The widest times stay inside the clock box (x 4..195). */
static void test_clock_fits_the_panel(void)
{
    static const struct { uint32_t utc; bool h24; } cases[] = {
        {T_14_27_05 + 6 * 3600 + 33 * 60, true}, /* 20:00, wide digits */
        {T_14_27_05 + 18 * 3600 + 32 * 60, true}, /* 08:59 */
        {T_14_27_05 - 2 * 3600 + 31 * 60, false}, /* 12:58 PM */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ui_config_t c = cfg(1);
        c.clock_24h = cases[i].h24;
        ui_set_config(&c);
        ui_set_time(cases[i].utc);
        run(2100);
        assert_inside(4, 192, 40, 130);
    }
}

/* The widest values the SHTC3 can report stay inside the box (x 4..195): the sensor row sits at y 120..150
 * (with the climate row and no weather the gaps are 40 px; the row ends well above the rounded corners). */
static void test_climate_values_fit_their_boxes(void)
{
    static const struct { int16_t centi_c; uint8_t unit; } cases[] = {
        {-4500, PROTO_TEMP_UNIT_CELSIUS},     /* -45.0°C, the sensor's minimum */
        {12500, PROTO_TEMP_UNIT_FAHRENHEIT},  /* 257.0°F, its maximum */
        {-4500, PROTO_TEMP_UNIT_FAHRENHEIT},  /* -49.0°F */
    };
    ui_deinit();
    start(true);
    ui_set_time(T_14_27_05);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ui_config_t c = cfg(1);
        c.temp_unit = cases[i].unit;
        ui_set_config(&c);
        board_climate_t cl = {.temp_centi_c = cases[i].centi_c, .rh_centi_pct = 10000};
        ui_set_climate(&cl);
        run(2100);
        assert_inside(4, 192, 100, 160);
    }
}

static void test_climate_line(void)
{
    ui_deinit();
    start(true);
    ui_set_time(T_14_27_05);
    run(2100);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Sunday, 11 Oct\n--.-°C\n--%\n")); /* no reading yet */
    board_climate_t cl = {.temp_centi_c = 2373, .rh_centi_pct = 6297};
    ui_set_climate(&cl);
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "\n23.7°C\n63%\n"));
    ui_stats_t a, b;
    ui_get_stats(&a);
    cl.temp_centi_c = 2374; /* still 23.7: no redraw */
    cl.rh_centi_pct = 6340;
    ui_set_climate(&cl);
    run(400);
    ui_get_stats(&b);
    TEST_ASSERT_EQUAL_UINT32(a.partial + a.full + a.skipped, b.partial + b.full + b.skipped);
    ui_config_t c = cfg(10);
    c.temp_unit = PROTO_TEMP_UNIT_FAHRENHEIT;
    ui_set_config(&c);
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "\n74.7°F\n63%\n")); /* 23.74 C = 74.73 F */
    c.temp_unit = PROTO_TEMP_UNIT_CELSIUS;
    ui_set_config(&c);
    cl.temp_centi_c = -5; /* rounds away from zero */
    cl.rh_centi_pct = 10000;
    ui_set_climate(&cl);
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "\n-0.1°C\n100%\n"));
    cl.temp_centi_c = -1234;
    ui_set_climate(&cl);
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "\n-12.3°C\n100%\n"));
    ui_set_climate(NULL); /* the sensor failed */
    run(400);
    TEST_ASSERT_NOT_NULL(strstr(text(), "\n--.-°C\n--%\n"));
    npipe_notif_t n = notif(1, "Zalo", "Hi", "");
    ui_on_notification(&n);
    run(400);
    TEST_ASSERT_NULL(strstr(text(), "°C")); /* idle screen only */
}

/* ---------------------------------------------------------------- status bar: connection icon */

/* The 16x16 icon whose top-left is (x0, y0) on the panel equals the generated bitmap. */
static bool link_icon_at(uint32_t x0, uint32_t y0, ui_status_icon_t which)
{
    for (uint32_t y = 0; y < UI_STATUS_ICON_SIZE; y++) {
        for (uint32_t x = 0; x < UI_STATUS_ICON_SIZE; x++) {
            const bool ink = !((ui_status_icons[which][y * 2 + x / 8] >> (7 - x % 8)) & 1);
            if (pixel_black(x0 + x, y0 + y) != ink) {
                return false;
            }
        }
    }
    return true;
}

static void test_connection_icon(void)
{
    ui_status_t st = {.battery_pct = 80, .ota_pct = -1};
    ui_set_status(&st);
    run(2100);
    /* "80%" is right-aligned at x 196; the icon sits 4 px left of it, centred in the 24 px bar */
    TEST_ASSERT_EQUAL_INT(UI_LINK_DISCONNECTED, ui_link_icon());
    lv_point_t ts;
    lv_text_get_size(&ts, "80%", &ui_font_regular_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const uint32_t x0 = SIM_EPAPER_W - 4 - (uint32_t)ts.x - 4 - UI_STATUS_ICON_SIZE, y0 = SIM_EPAPER_H - 24 + 4;
    TEST_ASSERT_TRUE(link_icon_at(x0, y0, UI_STATUS_ICON_BLE_DISCONNECTED));
    st.connected = true;
    ui_set_status(&st);
    run(400);
    TEST_ASSERT_EQUAL_INT(UI_LINK_CONNECTED, ui_link_icon());
    TEST_ASSERT_TRUE(link_icon_at(x0, y0, UI_STATUS_ICON_BLE_CONNECTED));
    /* the longest left text is clipped before the icon */
    ui_config_t c = cfg(10);
    c.language = PROTO_LANGUAGE_VI;
    ui_set_config(&c);
    st.ota_pct = 100;
    st.external_power = true;
    ui_set_status(&st);
    run(400);
    lv_text_get_size(&ts, "80%", &ui_font_regular_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    /* charging: the bolt sits left of the level (2 px gap), the link icon left of the bolt (4 px gap) */
    const uint32_t bolt_x = SIM_EPAPER_W - 4 - (uint32_t)ts.x - 2 - UI_STATUS_ICON_SIZE;
    TEST_ASSERT_TRUE(link_icon_at(bolt_x, y0, UI_STATUS_ICON_CHARGING));
    TEST_ASSERT_TRUE(link_icon_at(bolt_x - 4 - UI_STATUS_ICON_SIZE, y0, UI_STATUS_ICON_BLE_CONNECTED));
    st.external_power = false;
    ui_set_status(&st);
    run(400);
    TEST_ASSERT_TRUE(link_icon_at(SIM_EPAPER_W - 4 - (uint32_t)ts.x - 4 - UI_STATUS_ICON_SIZE, y0,
                                  UI_STATUS_ICON_BLE_CONNECTED)); /* no bolt: the link icon moves back */
    TEST_ASSERT_FALSE(link_icon_at(bolt_x, y0, UI_STATUS_ICON_CHARGING));
    ui_set_power(false);
    run(2100);
    TEST_ASSERT_EQUAL_INT(UI_LINK_NONE, ui_link_icon());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_boot_is_one_full_refresh);
    RUN_TEST(test_full_refresh_every);
    RUN_TEST(test_full_refresh_cards_count_a_quarter);
    RUN_TEST(test_full_refresh_waits_for_a_quiet_moment);
    RUN_TEST(test_partial_takes_300ms);
    RUN_TEST(test_changes_during_a_refresh_are_coalesced);
    RUN_TEST(test_unchanged_frame_is_not_refreshed);
    RUN_TEST(test_cards_and_buttons);
    RUN_TEST(test_keys_step_back_to_clock);
    RUN_TEST(test_settings_navigation);
    RUN_TEST(test_settings_select_changes_values);
    RUN_TEST(test_settings_actions);
    RUN_TEST(test_settings_pages);
    RUN_TEST(test_settings_power_off_timeout_and_notifications);
    RUN_TEST(test_setup_screen_qr);
    RUN_TEST(test_weather_row);
    RUN_TEST(test_wifi_status_icon);
    RUN_TEST(test_setting_notice);
    RUN_TEST(test_cards_time_out_to_the_clock);
    RUN_TEST(test_card_stays_before_the_next_one_shows);
    RUN_TEST(test_pairing_screen_and_keys);
    RUN_TEST(test_badge_and_clock_keys);
    RUN_TEST(test_orientation_turns_the_picture);
    RUN_TEST(test_keeps_the_newest_cards);
    RUN_TEST(test_status_language_and_privacy);
    RUN_TEST(test_passkey_screen);
    RUN_TEST(test_mask_numbers);
    RUN_TEST(test_card_masks_numbers_when_asked);
    RUN_TEST(test_power_off_screen);
    RUN_TEST(test_builtin_icon_per_category);
    RUN_TEST(test_no_icon_without_a_card);
    RUN_TEST(test_app_icon_replaces_the_category_icon);
    RUN_TEST(test_icon_installed_or_replaced_later_applies_at_the_next_render);
    RUN_TEST(test_malformed_icon_files_are_ignored);
    RUN_TEST(test_icon_without_blob_storage);
    RUN_TEST(test_long_title_stays_on_one_line_beside_the_icon);
    RUN_TEST(test_idle_clock_redraws_once_a_minute);
    RUN_TEST(test_clock_time_zone_and_date_rollover);
    RUN_TEST(test_clock_12_hour);
    RUN_TEST(test_clock_fits_the_panel);
    RUN_TEST(test_climate_values_fit_their_boxes);
    RUN_TEST(test_connection_icon);
    RUN_TEST(test_climate_line);
    return UNITY_END();
}
