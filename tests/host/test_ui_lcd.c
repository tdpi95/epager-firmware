/* ui_common on the simulated LCD (PLAN D51-D55): the area backend sends only what changed,
 * the colour layout's card model, keys, touch gestures (swipe, dismiss, tap), the backlight policy
 * (dim, off, wake, the waking touch swallowed), colour app icons, the weather row, the pairing screen and the
 * Wi-Fi setup screen. Pixels: golden images. */
#include <stdio.h>
#include <string.h>

#include "epager/blob_storage.h"
#include "epager/proto_gen.h"
#include "epager/sim_lcd.h"
#include "epager/ui.h"
#include "epager/xfer.h"
#include "face_test.h"
#include "lvgl.h"
#include "unity.h"

LV_FONT_DECLARE(ui_font_lcd_16)
LV_FONT_DECLARE(ui_font_lcd_20)
LV_FONT_DECLARE(ui_font_lcd_bold_20)
LV_FONT_DECLARE(ui_font_emoji_colour)

#define FULL_FRAME ((uint64_t)SIM_LCD_W * SIM_LCD_H)

static uint32_t now;
static blob_mem_t blob;
static ui_action_t last_action;
static uint32_t last_arg, n_actions;

static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return now;
}

static void on_action(void *ctx, ui_action_t a, uint32_t arg)
{
    (void)ctx;
    last_action = a;
    last_arg = arg;
    n_actions++;
}

static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        now += 10;
        (void)ui_service();
    }
}

static ui_config_t cfg(void)
{
    return (ui_config_t){.language = PROTO_LANGUAGE_EN, .show_body = true, .clock_24h = true, .brightness = 70,
                         .idle_dim_s = 30, .idle_off_s = 300};
}

static sim_lcd_state_t state(void)
{
    sim_lcd_state_t s;
    sim_lcd_state(&s);
    return s;
}

void setUp(void)
{
    now = 1000;
    last_action = UI_ACTION_NONE;
    n_actions = 0;
    sim_lcd_reset();
    blob_mem_init(&blob, 16384);
    const board_profile_t *b = board_get();
    ui_params_t p = {.display = b->display, .blob = &blob.iface, .now_ms = now_ms, .read_touch = b->read_touch,
                     .on_action = on_action};
    TEST_ASSERT_TRUE(ui_init(&p));
    ui_config_t c = cfg();
    ui_set_config(&c);
}

void tearDown(void)
{
    ui_deinit();
    blob_mem_free(&blob);
}

static npipe_notif_t notif(uint32_t id, uint8_t category, const char *app, const char *title, const char *body)
{
    npipe_notif_t n;
    memset(&n, 0, sizeof(n));
    n.id = id;
    n.kind = NPIPE_ADDED;
    n.shown = true;
    n.category = category;
    snprintf(n.app_id, sizeof(n.app_id), "com.example.%s", app);
    snprintf(n.app_name, sizeof(n.app_name), "%s", app);
    snprintf(n.title, sizeof(n.title), "%s", title);
    snprintf(n.body, sizeof(n.body), "%s", body);
    n.app_id_len = (uint16_t)strlen(n.app_id);
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

/* A finger moving from (x0, y0) to (x1, y1) in `steps` polls, then lifting. */
static void drag(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, int steps)
{
    for (int i = 0; i <= steps; i++) {
        sim_lcd_touch(true, (uint16_t)(x0 + (x1 - x0) * i / steps), (uint16_t)(y0 + (y1 - y0) * i / steps));
        run(20);
    }
    sim_lcd_touch(false, 0, 0);
    run(60);
}

/* ---------------------------------------------------------------- backend */

static void test_first_frame_then_backlight(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, state().backlight); /* nothing on the panel yet */
    run(20);
    sim_lcd_state_t s = state();
    TEST_ASSERT_TRUE(s.inited);
    /* the whole screen in bands; LVGL draws the empty screen once before the layout fills it in, and the
       clock's first move to its centred place redraws its box */
    TEST_ASSERT_TRUE(s.pixels >= FULL_FRAME && s.pixels <= 2 * FULL_FRAME + FULL_FRAME / 4);
    TEST_ASSERT_TRUE(s.writes > 1);
    TEST_ASSERT_EQUAL_UINT8(70, s.backlight);
    TEST_ASSERT_EQUAL_UINT8(70, ui_backlight());
    TEST_ASSERT_EQUAL_UINT32(0, s.violations);
}

static void test_only_changes_are_sent(void)
{
    ui_set_time(1791700200); /* 14:30 UTC */
    run(20);
    sim_lcd_clear_counters();
    run(1000);
    TEST_ASSERT_EQUAL_UINT64(0, state().pixels); /* nothing changed: nothing sent */
    ui_set_time(1791700260);                     /* the next minute */
    run(20);
    const sim_lcd_state_t s = state();
    TEST_ASSERT_TRUE(s.pixels > 0);
    /* only the clock label's boxes (72 px digits, 99 px line; old and new place), not the screen */
    TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(FULL_FRAME / 2, s.pixels, "a minute change must not resend the screen");
    TEST_ASSERT_TRUE(s.last_y >= 24 && s.last_y + s.last_h <= 200); /* below the status bar, above the date */
}

/* ---------------------------------------------------------------- cards and keys */

static void test_card_masks_numbers_when_asked(void)
{
    npipe_notif_t n = notif(1, PROTO_CATEGORY_BUSINESS_AND_FINANCE, "Bank", "Balance 1.234.567", "Your code is 482913. Call 112");
    ui_on_notification(&n);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Balance 1.234.567\nYour code is 482913. Call 112"));
    ui_config_t c = cfg();
    c.mask_numbers = true;
    ui_set_config(&c);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Balance ****\nYour code is ****. Call 112"));
    c.mask_numbers = false;
    ui_set_config(&c);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Your code is 482913."));
}

static void test_card_and_keys(void)
{
    npipe_notif_t a = notif(1, PROTO_CATEGORY_SOCIAL, "Zalo", "Nguyễn Văn An", "Tối nay mấy giờ về?");
    npipe_notif_t b = notif(2, PROTO_CATEGORY_EMAIL, "Gmail", "Invoice", "Your invoice is ready");
    ui_on_notification(&a);
    ui_on_notification(&b);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Gmail\n1/2\nInvoice\nYour invoice is ready\n"));
    TEST_ASSERT_EQUAL_INT(UI_ICON_BUILTIN, ui_current_icon());
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_DOWN, BOARD_PRESS_SHORT));
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo\n2/2\nNguyễn Văn An\n"));
    ui_on_button(BOARD_BTN_UP, BOARD_PRESS_SHORT);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Gmail\n1/2\n"));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_BRIGHTNESS_UP, ui_on_button(BOARD_BTN_UP, BOARD_PRESS_LONG));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_BRIGHTNESS_DOWN, ui_on_button(BOARD_BTN_DOWN, BOARD_PRESS_LONG));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_PAIRING_TOGGLE, ui_on_button(BOARD_BTN_UP_DOWN, BOARD_PRESS_LONG));
    a.kind = NPIPE_REMOVED;
    b.kind = NPIPE_REMOVED;
    ui_on_notification(&a);
    ui_on_notification(&b);
    run(20);
    TEST_ASSERT_EQUAL_INT(UI_ICON_NONE, ui_current_icon());
    TEST_ASSERT_NULL(strstr(text(), "Gmail"));
}

static void test_colour_app_icon(void)
{
    npipe_notif_t a = notif(1, PROTO_CATEGORY_SOCIAL, "Zalo", "Hi", "");
    char id[XFER_APP_ICON_ID_LEN + 1], name[BLOB_NAME_MAX + 1];
    xfer_app_icon_id(a.app_id, a.app_id_len, id);
    xfer_asset_blob_name(PROTO_ASSET_TYPE_APP_ICON, id, XFER_APP_ICON_ID_LEN, name);
    static uint8_t file[XFER_ASSET_HEADER_LEN + 3 * 32 * 32];
    memset(file, 0x80, sizeof(file));
    memcpy(file, "ESPA\x01\x05\x06\x00\x20\x00\x20\x00\x00\x0c\x00\x00", XFER_ASSET_HEADER_LEN); /* 32x32, 3072 */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, blob.iface.write(blob.iface.ctx, name, 0, file, sizeof(file)));
    ui_on_notification(&a);
    run(20);
    TEST_ASSERT_EQUAL_INT(UI_ICON_APP, ui_current_icon());
    file[6] = PROTO_ASSET_FORMAT_MONO1; /* wrong format: the built-in icon is used */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, blob.iface.write(blob.iface.ctx, name, 0, file, sizeof(file)));
    ui_assets_changed(PROTO_ASSET_TYPE_APP_ICON);
    run(20);
    TEST_ASSERT_EQUAL_INT(UI_ICON_BUILTIN, ui_current_icon());
}

/* ---------------------------------------------------------------- touch */

static void test_swipes_and_dismiss(void)
{
    npipe_notif_t a = notif(11, PROTO_CATEGORY_SOCIAL, "Zalo", "A", "");
    npipe_notif_t b = notif(12, PROTO_CATEGORY_SOCIAL, "Viber", "B", "");
    ui_on_notification(&a);
    ui_on_notification(&b); /* newest first: B is shown */
    run(20);
    drag(220, 120, 60, 120, 6); /* swipe left: next card */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo\n2/2\n"));
    drag(60, 120, 220, 120, 6); /* swipe right: previous */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Viber\n1/2\n"));
    TEST_ASSERT_EQUAL_UINT32(0, n_actions);
    drag(148, 60, 148, 200, 6); /* swipe down: dismiss the card on screen */
    TEST_ASSERT_EQUAL_UINT32(1, n_actions);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_DISMISS, last_action);
    TEST_ASSERT_EQUAL_UINT32(12, last_arg);
}

/* True if `line` is a whole line of the screen's text. */
static bool has_line(const char *line)
{
    char want[32];
    snprintf(want, sizeof(want), "\n%s\n", line);
    char all[UI_TEXT_MAX + 2];
    snprintf(all, sizeof(all), "\n%s", text());
    return strstr(all, want) != NULL;
}

/* PLAN D70: swipe up on a card goes back to the clock screen (the notifications stay, a badge counts them),
 * swipe down there brings them back, and a key does the same. */
static void test_hide_cards_and_the_badge(void)
{
    npipe_notif_t a = notif(11, PROTO_CATEGORY_SOCIAL, "Zalo", "A", "");
    npipe_notif_t b = notif(12, PROTO_CATEGORY_EMAIL, "Mail", "B", "");
    ui_on_notification(&a);
    ui_on_notification(&b);
    ui_set_time(1791700215);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail\n1/2\n"));
    drag(148, 200, 148, 60, 6); /* swipe up: back to the clock screen */
    TEST_ASSERT_NULL(strstr(text(), "Mail"));
    TEST_ASSERT_TRUE(has_line("2")); /* the badge: two notifications wait */
    TEST_ASSERT_EQUAL_UINT32(0, n_actions);          /* hiding is not dismissing */
    drag(60, 120, 220, 120, 6);                      /* swipe right: nothing */
    drag(220, 120, 60, 120, 6);                      /* swipe left: nothing */
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
    TEST_ASSERT_EQUAL_UINT32(0, n_actions);

    drag(148, 60, 148, 200, 6); /* swipe down: the cards again, the newest first */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail\n1/2\n"));
    drag(148, 200, 148, 60, 6);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_DOWN, BOARD_PRESS_SHORT)); /* a key shows them too */
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail\n1/2\n"));

    drag(148, 200, 148, 60, 6);
    npipe_notif_t c = notif(13, PROTO_CATEGORY_SOCIAL, "Line", "C", "");
    ui_on_notification(&c); /* a new notification opens its card from the clock screen */
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Line\n1/3\n"));

    drag(148, 200, 148, 60, 6);
    c.kind = NPIPE_REMOVED; /* the phone removes one while the clock shows: the badge follows */
    ui_on_notification(&c);
    run(20);
    TEST_ASSERT_TRUE(has_line("2"));
    a.kind = NPIPE_REMOVED;
    b.kind = NPIPE_REMOVED;
    ui_on_notification(&a);
    ui_on_notification(&b);
    run(20);
    TEST_ASSERT_FALSE(has_line("2")); /* none left: no badge, the clock alone */
    drag(148, 60, 148, 200, 6);                  /* swipe down with nothing waiting: nothing */
    TEST_ASSERT_NULL(strstr(text(), "Zalo"));
}

/* A tap shows all of the title and body (head included); a card taller than the screen scrolls by a vertical
 * drag, which then neither dismisses the card nor goes back to the clock. A second tap collapses it. */
static void test_tap_shows_the_whole_card(void)
{
    char body[600] = "";
    for (int i = 0; i < 40; i++) {
        strcat(body, "word ");
    }
    npipe_notif_t a = notif(1, PROTO_CATEGORY_SOCIAL, "Zalo", "A title long enough to need a second line on screen", body);
    ui_on_notification(&a);
    run(20);
    drag(148, 200, 148, 60, 6); /* collapsed: swipe up goes back to the clock screen */
    TEST_ASSERT_NULL(strstr(text(), "Zalo")); /* the card is hidden */
    drag(148, 60, 148, 200, 6);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo"));

    drag(150, 150, 150, 150, 2); /* tap */
    TEST_ASSERT_NOT_NULL(strstr(text(), "A title long enough to need a second line on screen"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "word word"));
    drag(148, 200, 148, 60, 6); /* scrolls: the card stays */
    drag(148, 60, 148, 200, 6);
    drag(148, 200, 148, 60, 6);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo"));
    TEST_ASSERT_EQUAL_UINT32(0, n_actions);
    drag(60, 120, 220, 120, 6); /* sideways still steps cards (only one here) */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo"));
    drag(150, 150, 150, 150, 2); /* tap: collapsed again */
    drag(148, 200, 148, 60, 6);
    TEST_ASSERT_NULL(strstr(text(), "Zalo")); /* the card is hidden */

    drag(148, 60, 148, 200, 6);
    npipe_notif_t b = notif(2, PROTO_CATEGORY_SOCIAL, "Viber", "Short", "Short body");
    ui_on_notification(&b);
    run(2100); /* the first card has been shown for its minimum time: the newer one opens */
    drag(150, 150, 150, 150, 2); /* expanded but it all fits: vertical swipes keep their meaning */
    drag(148, 60, 148, 200, 6);
    TEST_ASSERT_EQUAL_UINT32(1, n_actions);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_DISMISS, last_action);
}

/* ---------------------------------------------------------------- weather, pairing, Wi-Fi setup */

static wx_reading_t reading(int16_t t, int16_t hi, int16_t lo, uint8_t rain)
{
    return (wx_reading_t){.temp_c10 = t, .high_c10 = hi, .low_c10 = lo, .precip_pct = rain,
                          .cond = WX_COND_PARTLY_CLOUDY, .is_day = true};
}

static void test_weather_row(void)
{
    ui_set_time(1791700200);
    run(20);
    TEST_ASSERT_NULL(strstr(text(), "\u00B0"));
    wx_reading_t r = reading(312, 321, 246, 86);
    ui_set_weather(&r);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "31\u00B0\n32\u00B0 / 25\u00B0   \u2614" "86%\n"));
    TEST_ASSERT_NULL(strstr(text(), "Hà Nội")); /* no place: no line for it */
    snprintf(r.place, sizeof(r.place), "%s", "Hà Nội");
    ui_set_weather(&r);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "\u2614" "86%\nHà Nội\n")); /* under the row */
    sim_lcd_clear_counters();
    r.temp_c10 = 308; /* still 31 deg: nothing is redrawn */
    ui_set_weather(&r);
    run(100);
    TEST_ASSERT_EQUAL_UINT64(0, state().pixels);

    ui_config_t c = cfg();
    c.temp_unit = PROTO_TEMP_UNIT_FAHRENHEIT;
    ui_set_config(&c);
    r = reading(-34, WX_TEMP_UNKNOWN, -55, WX_PCT_UNKNOWN);
    ui_set_weather(&r);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "26\u00B0\n")); /* no high: no high/low, no rain: nothing more */
    TEST_ASSERT_NULL(strstr(text(), " / "));
    r.precip_pct = 0;
    ui_set_weather(&r);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "26\u00B0\n\u2614" "0%\n"));

    /* a card covers the idle screen, weather included */
    npipe_notif_t a = notif(1, PROTO_CATEGORY_SOCIAL, "Zalo", "Hi", "");
    ui_on_notification(&a);
    run(20);
    TEST_ASSERT_NULL(strstr(text(), "\u00B0"));
    a.kind = NPIPE_REMOVED;
    ui_on_notification(&a);
    ui_set_weather(NULL);
    run(20);
    TEST_ASSERT_NULL(strstr(text(), "\u00B0"));
}

static void test_wifi_status_icon(void)
{
    ui_status_t st = {.battery_pct = 80, .ota_pct = -1};
    ui_set_status(&st);
    run(20);
    TEST_ASSERT_EQUAL_INT(UI_WIFI_NONE, ui_wifi_icon()); /* no network stored */
    const struct {
        uint8_t state;
        ui_wifi_icon_t icon;
    } cases[] = {
        {PROTO_WIFI_STATE_IDLE, UI_WIFI_NONE},         {PROTO_WIFI_STATE_CONNECTING, UI_WIFI_NONE},
        {PROTO_WIFI_STATE_CONNECTED, UI_WIFI_OK},      {PROTO_WIFI_STATE_AUTH_FAILED, UI_WIFI_NONE},
        {PROTO_WIFI_STATE_NOT_FOUND, UI_WIFI_NONE},    {PROTO_WIFI_STATE_NO_INTERNET, UI_WIFI_NONE},
        {200, UI_WIFI_NONE}, /* a newer state: no icon rather than a wrong one */
        {PROTO_WIFI_STATE_UNCONFIGURED, UI_WIFI_NONE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        st.wifi_state = cases[i].state;
        ui_set_status(&st);
        run(20);
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].icon, ui_wifi_icon(), "wifi_state");
    }
    st.wifi_state = PROTO_WIFI_STATE_CONNECTED;
    ui_set_status(&st);
    ui_set_power(false);
    run(20);
    TEST_ASSERT_EQUAL_INT(UI_WIFI_NONE, ui_wifi_icon()); /* the bar is hidden when powered off */
    ui_set_power(true);
    run(20);
    TEST_ASSERT_EQUAL_INT(UI_WIFI_OK, ui_wifi_icon());
    st.wifi_state = PROTO_WIFI_STATE_UNCONFIGURED;
    ui_set_status(&st);
    run(20);
}

static void reinit(bool wifi_setup)
{
    ui_deinit();
    const board_profile_t *b = board_get();
    ui_params_t p = {.display = b->display, .blob = &blob.iface, .now_ms = now_ms, .read_touch = b->read_touch,
                     .on_action = on_action, .wifi_setup = wifi_setup};
    TEST_ASSERT_TRUE(ui_init(&p));
    ui_config_t c = cfg();
    ui_set_config(&c);
}

/* A card nobody touches gives way to the clock screen after cards_timeout_s; touching, keys and new
 * notifications start the time again; the notification itself stays. */
static void test_cards_time_out(void)
{
    ui_deinit();
    const board_profile_t *b = board_get();
    ui_params_t p = {.display = b->display, .blob = &blob.iface, .now_ms = now_ms, .read_touch = b->read_touch,
                     .on_action = on_action, .cards_timeout_s = UI_CARDS_TIMEOUT_S};
    TEST_ASSERT_TRUE(ui_init(&p));
    ui_config_t c = cfg();
    c.idle_dim_s = 0;
    c.idle_off_s = 0;
    ui_set_config(&c);

    npipe_notif_t a = notif(11, PROTO_CATEGORY_SOCIAL, "Zalo", "A", "");
    ui_on_notification(&a);
    ui_set_time(1791700215);
    run(50000);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo")); /* 50 s: still there */
    ui_activity();                                /* a key or a touch */
    run(50000);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo")); /* 100 s since the start, 50 s since the touch */
    run(11000);
    TEST_ASSERT_NULL(strstr(text(), "Zalo"));     /* a minute without a touch: the clock screen */
    TEST_ASSERT_TRUE(has_line("1"));              /* the badge: the notification waits */
    TEST_ASSERT_EQUAL_UINT32(0, n_actions);       /* nothing was dismissed */

    drag(148, 60, 148, 200, 6);                   /* swipe down: the card is back, for another minute */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo"));
    run(61000);
    TEST_ASSERT_NULL(strstr(text(), "Zalo"));
    npipe_notif_t b2 = notif(12, PROTO_CATEGORY_EMAIL, "Mail", "B", "");
    ui_on_notification(&b2);                      /* a new one opens its card again */
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Mail"));
}

static void test_pairing_screen(void)
{
    npipe_notif_t a = notif(5, PROTO_CATEGORY_SOCIAL, "Zalo", "Hidden card", "");
    ui_on_notification(&a);
    ui_status_t st = {.battery_pct = 80, .external_power = true, .pairing_mode = true, .ota_pct = -1};
    ui_set_status(&st);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Pairing mode\n\u26A1 80%\nPairing mode\nOpen the ePager app"));
    TEST_ASSERT_NULL(strstr(text(), "USB")); /* external power is an icon, not text */
    TEST_ASSERT_NULL(strstr(text(), "Hidden card"));
    TEST_ASSERT_NULL(strstr(text(), "Wi-Fi setup")); /* not offered without Wi-Fi */
    drag(148, 200, 148, 60, 6);                      /* swipe up: nothing happens to the hidden card */
    drag(148, 194, 148, 194, 2);
    TEST_ASSERT_EQUAL_UINT32(0, n_actions);

    reinit(true);
    ui_set_status(&st);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Wi-Fi setup"));
    drag(148, 194, 148, 194, 2); /* the button */
    TEST_ASSERT_EQUAL_UINT32(1, n_actions);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_WIFI_SETUP, last_action);
    drag(148, 100, 148, 100, 2); /* the text above it: nothing */
    TEST_ASSERT_EQUAL_UINT32(1, n_actions);
    st.pairing_mode = false;
    ui_set_status(&st);
    run(20);
    TEST_ASSERT_NULL(strstr(text(), "Wi-Fi setup"));
}

/* A phone is pairing: the passkey it asks for replaces the hint and the buttons (PLAN D6: DisplayOnly). */
static void test_pairing_passkey(void)
{
    reinit(true);
    ui_status_t st = {.battery_pct = 80, .pairing_mode = true, .ota_pct = -1};
    ui_set_status(&st);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Wi-Fi setup"));
    st.has_passkey = true;
    st.passkey = 4821;
    ui_set_status(&st);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Pairing mode\n004821\nEnter this code on your phone\n"));
    TEST_ASSERT_NULL(strstr(text(), "Open the ePager app"));
    TEST_ASSERT_NULL(strstr(text(), "Wi-Fi setup")); /* no button to tap while a phone pairs */
    drag(148, 194, 148, 194, 2);
    TEST_ASSERT_EQUAL_UINT32(0, n_actions);
    st.has_passkey = false;
    ui_set_status(&st);
    run(20);
    TEST_ASSERT_NULL(strstr(text(), "004821"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Wi-Fi setup"));
}

static void test_wifi_setup_screen(void)
{
    reinit(true);
    ui_wifi_setup_t s = {.state = UI_SETUP_WAITING};
    snprintf(s.ap_ssid, sizeof(s.ap_ssid), "ePager-1A2B");
    snprintf(s.ap_password, sizeof(s.ap_password), "k7m2p9qx4t");
    snprintf(s.url, sizeof(s.url), "192.168.4.1");
    ui_set_wifi_setup(&s);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Join Wi-Fi\nePager-1A2B\nPassword\nk7m2p9qx4t\nthen open\n192.168.4.1\nClose\n"));
    s.state = UI_SETUP_JOINING;
    snprintf(s.network, sizeof(s.network), "Quán Cafe 5G");
    ui_set_wifi_setup(&s);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Connecting to\nQuán Cafe 5G ..."));
    TEST_ASSERT_NULL(strstr(text(), "k7m2p9qx4t"));
    /* Close, or any short or long key, closes it */
    drag(148, 194, 148, 194, 2);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_WIFI_SETUP_CLOSE, last_action);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_WIFI_SETUP_CLOSE, ui_on_button(BOARD_BTN_UP, BOARD_PRESS_SHORT));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_WIFI_SETUP_CLOSE, ui_on_button(BOARD_BTN_DOWN, BOARD_PRESS_LONG));
    s.state = UI_SETUP_OFF;
    ui_set_wifi_setup(&s);
    run(20);
    TEST_ASSERT_NULL(strstr(text(), "Close"));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_BRIGHTNESS_DOWN, ui_on_button(BOARD_BTN_DOWN, BOARD_PRESS_LONG));
}

/* ---------------------------------------------------------------- LVGL pool */

/* The heaviest screens in a row: a long emoji card (each LZ4 emoji is unpacked into a pool buffer while it
 * is drawn), the weather row and the setup screen. LV_MEM_SIZE is 48 KB and shared with the Paper. */
static void test_lvgl_pool_headroom(void)
{
    reinit(true);
    npipe_notif_t a = notif(1, PROTO_CATEGORY_SOCIAL, "Messenger", "Party \U0001F389\U0001F389 tonight \u2705",
                            "\U0001F600\u2764\U0001F525\U0001F44D\U0001F389\U0001F382\U0001F381\U0001F388 "
                            "Gia \u0111\u00ECnh \U0001F1FB\U0001F1F3 \u0111\u00E3 s\u1EB5n s\u00E0ng \U0001F60D\U0001F64F"
                            "\U0001F60E\U0001F914\U0001F44F\U0001F680\u2600\U0001F327");
    ui_on_notification(&a);
    run(40);
    drag(150, 150, 150, 150, 2); /* the whole body */
    a.kind = NPIPE_REMOVED;
    ui_on_notification(&a);
    ui_set_time(1791700200);
    wx_reading_t r = reading(312, 321, 246, 86);
    ui_set_weather(&r);
    run(40);
    ui_wifi_setup_t s = {.state = UI_SETUP_WAITING};
    snprintf(s.ap_ssid, sizeof(s.ap_ssid), "ePager-1A2B");
    snprintf(s.ap_password, sizeof(s.ap_password), "k7m2p9qx4t");
    snprintf(s.url, sizeof(s.url), "192.168.4.1");
    ui_set_wifi_setup(&s);
    run(40);
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    printf("LVGL pool: %u of %u bytes at most, %u now\n", (unsigned)m.max_used, (unsigned)m.total_size,
           (unsigned)(m.total_size - m.free_size));
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(m.total_size - 8u * 1024u, m.max_used); /* keep 8 KB spare */
}

/* ---------------------------------------------------------------- backlight */

static void test_dim_off_and_wake(void)
{
    run(20);
    run(29000);
    TEST_ASSERT_EQUAL_UINT8(70, state().backlight);
    run(2000); /* 31 s idle */
    TEST_ASSERT_EQUAL_UINT8(20, state().backlight);
    run(270000); /* 301 s idle */
    TEST_ASSERT_EQUAL_UINT8(0, state().backlight);
    TEST_ASSERT_TRUE(state().asleep);
    npipe_notif_t a = notif(1, PROTO_CATEGORY_SOCIAL, "Zalo", "While asleep", "");
    ui_on_notification(&a); /* a new notification wakes the screen */
    run(20);
    TEST_ASSERT_FALSE(state().asleep);
    TEST_ASSERT_EQUAL_UINT8(70, state().backlight);
    TEST_ASSERT_EQUAL_UINT32(0, state().violations); /* nothing was written while asleep */
    TEST_ASSERT_NOT_NULL(strstr(text(), "While asleep"));
}

static void test_waking_touch_does_nothing_else(void)
{
    npipe_notif_t a = notif(1, PROTO_CATEGORY_SOCIAL, "Zalo", "A", "");
    npipe_notif_t b = notif(2, PROTO_CATEGORY_SOCIAL, "Viber", "B", "");
    ui_on_notification(&a);
    ui_on_notification(&b);
    run(301000);
    TEST_ASSERT_TRUE(state().asleep);
    drag(220, 120, 60, 120, 6); /* this swipe only wakes the screen */
    TEST_ASSERT_FALSE(state().asleep);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Viber\n1/2\n"));
    run(301000);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_DOWN, BOARD_PRESS_SHORT)); /* only wakes */
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Viber\n1/2\n"));
}

/* The rows of the settings page on screen: y of the middle of row k (SET_ROW_Y 56, 40 px a row). */
#define ROW_Y(k) (76 + 40 * (k))
#define SWIPE_UP() drag(148, 200, 148, 60, 6)
#define SWIPE_DOWN() drag(148, 60, 148, 200, 6)
#define SWIPE_LEFT() drag(220, 120, 60, 120, 6)
#define SWIPE_BACK() drag(60, 120, 220, 120, 6) /* swipe right */
#define TAP_ROW(k) drag(148, ROW_Y(k), 148, ROW_Y(k), 2)

static const char *PAGE1 = "Settings\n1/3\nClock\n24h\nTemperature\n\xC2\xB0" "C\nLanguage\nEnglish\nMessage text\nOn\n";

/* PLAN D99: swipe up on the clock screen opens the settings, a swipe right goes back, up and down turn the pages. */
static void test_settings_open_pages_and_back(void)
{
    run(20);
    SWIPE_UP();
    TEST_ASSERT_NOT_NULL(strstr(text(), PAGE1));
    TEST_ASSERT_EQUAL_UINT32(0, n_actions);
    SWIPE_UP(); /* the next page: nine rows, four a page */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n2/3\nHide numbers\nOff\nRotation\n0\xC2\xB0\nPairing mode\nPaired phones\n>\n"));
    SWIPE_UP();
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n3/3\nClock face\n>\nAbout\n>\n"));
    SWIPE_UP(); /* the last page stays */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n3/3\nClock face\n"));
    SWIPE_DOWN();
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n2/3\n"));
    SWIPE_DOWN();
    SWIPE_DOWN(); /* the first stays */
    TEST_ASSERT_NOT_NULL(strstr(text(), PAGE1));
    SWIPE_LEFT(); /* swipe left does nothing */
    TEST_ASSERT_NOT_NULL(strstr(text(), PAGE1));
    SWIPE_BACK(); /* back: the clock screen */
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
    TEST_ASSERT_EQUAL_UINT32(0, n_actions);

    SWIPE_UP();
    TEST_ASSERT_NOT_NULL(strstr(text(), PAGE1)); /* each visit starts on the first page */
}

static void test_settings_tap_changes_a_value(void)
{
    run(20);
    SWIPE_UP();
    TAP_ROW(0); /* Clock: 24h -> 12h */
    TEST_ASSERT_EQUAL_UINT32(1, n_actions);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_SETTING, last_action);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_UI_CLOCK_24H << 8 | 0u, last_arg);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n1/3\n")); /* the list stays; the firmware reports the new value */
    ui_config_t c = cfg();
    c.clock_24h = false;
    ui_set_config(&c);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Clock\n12h\n"));
    TAP_ROW(2); /* Language: English -> Vietnamese */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_UI_LANGUAGE << 8 | PROTO_LANGUAGE_VI, last_arg);
    c.language = PROTO_LANGUAGE_VI;
    ui_set_config(&c);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "C\xC3\xA0i \xC4\x91\xE1\xBA\xB7t\n1/3\n\xC4\x90\xE1\xBB\x93ng h\xE1\xBB\x93\n12h\n")); /* Cài đặt, Đồng hồ */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Ti\xE1\xBA\xBFng Vi\xE1\xBB\x87t\n")); /* Tiếng Việt */
    c.language = PROTO_LANGUAGE_EN;
    ui_set_config(&c);

    SWIPE_UP();
    TAP_ROW(1); /* Rotation: the LCD turns by 0 or 180 degrees: 0 -> 180 */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_DISPLAY_ORIENTATION << 8 | 2u, last_arg);
    c.orientation = 2;
    ui_set_config(&c);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Rotation\n180\xC2\xB0\n"));
    TAP_ROW(1);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_DISPLAY_ORIENTATION << 8 | 0u, last_arg);
    c.orientation = 3; /* the phone set a quarter turn the panel cannot show: it counts as 180 */
    ui_set_config(&c);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Rotation\n180\xC2\xB0\n"));

    const uint32_t n = n_actions;
    drag(148, 228, 148, 228, 2); /* below the rows: nothing */
    TEST_ASSERT_EQUAL_UINT32(n, n_actions);
}

/* The Clock face pages by touch: the list (Classic, the built-in faces), a face's page, Use. */
static void test_settings_clock_face_pages(void)
{
    reinit(false);
    SWIPE_UP(); /* opens the list */
    SWIPE_UP();
    SWIPE_UP();
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n3/3\nClock face\n>\nAbout\n>\n"));
    TAP_ROW(0);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Clock face\nClassic\nIn use\nBig\nNight\n"));
    TAP_ROW(1); /* a built-in face: its page has Use this face and no Delete */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Big\nBig\nUse this face\n"));
    TEST_ASSERT_NULL(strstr(text(), "Delete"));
    const uint32_t n = n_actions;
    TAP_ROW(1);
    TEST_ASSERT_EQUAL_UINT32(n + 1, n_actions);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_FACE_SELECT, last_action);
    TEST_ASSERT_EQUAL_UINT32(1, last_arg);
    TEST_ASSERT_EQUAL_STRING("epager.big", ui_face_list_id(1));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Clock face\nClassic\nIn use\n")); /* back on the list */
    SWIPE_BACK(); /* to the settings, then the clock */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings"));
    SWIPE_BACK();
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
}

static void test_settings_pairing_and_pages(void)
{
    run(20);
    SWIPE_UP();
    SWIPE_UP();
    TAP_ROW(2); /* Pairing mode */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_PAIRING_TOGGLE, last_action);
    TEST_ASSERT_NULL(strstr(text(), "Settings")); /* the list closes; the firmware shows the pairing screen */
    ui_status_t st = {.battery_pct = 80, .pairing_mode = true, .ota_pct = -1};
    ui_set_status(&st);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Open the ePager app"));
    SWIPE_DOWN(); /* swipe down on the pairing screen: leave pairing mode */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_PAIRING_TOGGLE, last_action);
    TEST_ASSERT_EQUAL_UINT32(2, n_actions);
    st.pairing_mode = false;
    ui_set_status(&st);
    run(20);
    TEST_ASSERT_NULL(strstr(text(), "Settings")); /* and the list is not still there */

    ui_info_t info;
    memset(&info, 0, sizeof(info));
    info.n_phones = 2;
    info.phone_connected[1] = true;
    snprintf(info.firmware, sizeof(info.firmware), "%s", "1.2.3");
    snprintf(info.device, sizeof(info.device), "%s", "ePager-1A2B");
    snprintf(info.board, sizeof(info.board), "%s", "ostb_xiaozhi_3st");
    ui_set_info(&info);
    SWIPE_UP();
    SWIPE_UP();
    TAP_ROW(3); /* Paired phones */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_INFO_REFRESH, last_action);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Paired phones\nPhone 1\nPhone 2\nconnected\n"));
    TAP_ROW(0); /* asks */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Phone 1\nForget?\n"));
    const uint32_t n = n_actions;
    TAP_ROW(1); /* another row: the question moves there, nothing is forgotten */
    TEST_ASSERT_NULL(strstr(text(), "Phone 1\nForget?"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Phone 2\nForget?\n"));
    TEST_ASSERT_EQUAL_UINT32(n, n_actions);
    TAP_ROW(0);
    TAP_ROW(0); /* a second tap on the same row forgets it */
    TEST_ASSERT_EQUAL_UINT32(n + 1, n_actions);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_FORGET_PHONE, last_action);
    TEST_ASSERT_EQUAL_UINT32(0, last_arg);

    SWIPE_BACK(); /* back to the list, on the page of the row that opened it */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n2/3\nHide numbers\n"));
    SWIPE_UP();
    TAP_ROW(1); /* About (after Clock face) */
    TEST_ASSERT_NOT_NULL(strstr(text(), "About\n1/2\nFirmware\n1.2.3\nDevice\nePager-1A2B\nBoard\nxiaozhi_3st\nBattery\n"));
    TAP_ROW(0); /* read-only */
    TEST_ASSERT_NOT_NULL(strstr(text(), "About\n"));
    SWIPE_BACK();
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n3/3\nClock face\n>\nAbout\n"));
    SWIPE_BACK();
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
}

static void test_settings_closes(void)
{
    npipe_notif_t a = notif(1, PROTO_CATEGORY_SOCIAL, "Zalo", "A", "");
    run(20);
    SWIPE_UP();
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings"));
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_DOWN, BOARD_PRESS_SHORT)); /* a key closes it */
    run(20);
    TEST_ASSERT_NULL(strstr(text(), "Settings"));

    SWIPE_UP();
    ui_on_notification(&a); /* a new notification replaces it */
    run(20);
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Zalo"));
    SWIPE_UP(); /* a card on screen: swipe up goes back to the clock screen, never to the settings */
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
    SWIPE_UP(); /* then the clock screen: the settings */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings"));
    SWIPE_DOWN(); /* turns the page back, the list stays; it is the clock screen's swipe, not the cards' */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings"));
    TEST_ASSERT_EQUAL_UINT32(0, n_actions); /* nothing was dismissed */
}

/* On a board with Wi-Fi the list has the Weather, Wi-Fi setup and Wi-Fi networks rows too; the setup screen opens over
 * the list and the list is back when it closes. */
static void test_settings_with_wifi(void)
{
    reinit(true);
    SWIPE_UP();
    SWIPE_UP();
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n2/4\nHide numbers\nOff\nRotation\n0\xC2\xB0\nWeather\nOff\nPairing mode\n"));
    TAP_ROW(2); /* Weather */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_WEATHER_ENABLED << 8 | 1u, last_arg);
    SWIPE_UP();
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n3/4\nWi-Fi setup\nPaired phones\n>\nWi-Fi networks\n>\nClock face\n>\n"));
    TAP_ROW(0);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_WIFI_SETUP, last_action);
    ui_wifi_setup_t s = {.state = UI_SETUP_WAITING};
    snprintf(s.ap_ssid, sizeof(s.ap_ssid), "ePager-1A2B");
    snprintf(s.ap_password, sizeof(s.ap_password), "k7m2p9qx4t");
    snprintf(s.url, sizeof(s.url), "192.168.4.1");
    ui_set_wifi_setup(&s);
    run(20);
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "k7m2p9qx4t"));
    SWIPE_BACK(); /* the setup screen has its own button */
    TEST_ASSERT_NOT_NULL(strstr(text(), "k7m2p9qx4t"));
    s.state = UI_SETUP_OFF;
    ui_set_wifi_setup(&s);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n3/4\nWi-Fi setup\n"));
}

/* ---------------------------------------------------------------- firmware update (docs/wifi-ota-plan.md) */

static void reinit_update(void)
{
    ui_deinit();
    const board_profile_t *b = board_get();
    ui_params_t p = {.display = b->display, .blob = &blob.iface, .now_ms = now_ms, .read_touch = b->read_touch,
                     .on_action = on_action, .wifi_setup = true, .update = true, .cards_timeout_s = UI_CARDS_TIMEOUT_S};
    TEST_ASSERT_TRUE(ui_init(&p));
    ui_config_t c = cfg();
    c.update_auto_check = true;
    ui_set_config(&c);
    last_action = UI_ACTION_NONE;
    run(20);
}

static ui_update_t upd_available(void)
{
    ui_update_t u = {.state = UI_UPD_AVAILABLE, .size = 2822144, .mark = true};
    snprintf(u.version, sizeof(u.version), "0.2.0");
    return u;
}

static void test_update_page_and_rows(void)
{
    reinit_update();
    SWIPE_UP(); /* opens the list */
    SWIPE_UP();
    SWIPE_UP();
    SWIPE_UP();
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n4/4\nSoftware update\n>\nClock face\n>\nAbout\n>\n"));
    SWIPE_BACK();
    SWIPE_UP();
    SWIPE_UP();
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings\n2/4\nHide numbers\nOff\nRotation\n0\xC2\xB0\nWeather\nOff\nCheck updates\nOn\n"));
    TAP_ROW(3); /* Check updates: a config key like the others */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PROTO_CFG_UPDATE_AUTO_CHECK << 8 | 0u, last_arg);
    SWIPE_UP();
    SWIPE_UP();
    TAP_ROW(0); /* Software update */
    TEST_ASSERT_NOT_NULL(strstr(text(), "Software update\nNot checked\nCheck now\n"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Check now"));
    TAP_ROW(1);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_UPDATE_CHECK, last_action);
    ui_update_t u = upd_available();
    ui_set_update(&u);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "New version"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "2.8 MB"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Install"));
    TAP_ROW(2); /* Install: the second row after the status and the size */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_UPDATE_INSTALL, last_action);
}

static void test_update_mark_and_prompt(void)
{
    reinit_update();
    ui_update_t u = upd_available();
    ui_set_update(&u);
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Update available")); /* the mark on the clock screen */
    TEST_ASSERT_TRUE(ui_show_update_prompt());
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Software update\nUpdate available\n0.2.0\nInstall\nRemind later\nSkip this version\n"));
    TAP_ROW(2); /* Remind later: reported, and the clock is back */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_UPDATE_LATER, last_action);
    TEST_ASSERT_NULL(strstr(text(), "Software update"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Update available")); /* the mark stays */
    TEST_ASSERT_TRUE(ui_show_update_prompt());
    run(20);
    last_action = UI_ACTION_NONE;
    SWIPE_BACK(); /* backing out reports nothing and is "remind later" too */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, last_action);
    TEST_ASSERT_NULL(strstr(text(), "Software update"));
    /* never on top of a notification, and not with the screen off */
    npipe_notif_t a = notif(1, PROTO_CATEGORY_SOCIAL, "Zalo", "Hello", "");
    ui_on_notification(&a);
    run(20);
    TEST_ASSERT_FALSE(ui_show_update_prompt());
    /* Skip this version, on the same page */
    reinit_update();
    ui_set_update(&u);
    TEST_ASSERT_TRUE(ui_show_update_prompt());
    run(20);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Skip this version")); /* the whole question is one page */
    TAP_ROW(3);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_UPDATE_SKIP, last_action);
    TEST_ASSERT_NULL(strstr(text(), "Software update"));
}

static void test_update_prompt_waits_for_an_awake_screen(void)
{
    reinit_update();
    ui_config_t c = cfg();
    c.update_auto_check = true;
    c.idle_dim_s = 0;
    c.idle_off_s = 60;
    ui_set_config(&c);
    ui_update_t u = upd_available();
    ui_set_update(&u);
    run(61000);
    TEST_ASSERT_TRUE(state().asleep);
    TEST_ASSERT_FALSE(ui_show_update_prompt()); /* it never wakes the screen */
    TEST_ASSERT_TRUE(state().asleep);
    ui_activity(); /* a key or a touch wakes it: now it may ask */
    run(20);
    TEST_ASSERT_TRUE(ui_show_update_prompt());
}

static void test_settings_close_by_idle(void)
{
    ui_deinit();
    const board_profile_t *b = board_get();
    ui_params_t p = {.display = b->display, .blob = &blob.iface, .now_ms = now_ms, .read_touch = b->read_touch,
                     .on_action = on_action, .cards_timeout_s = UI_CARDS_TIMEOUT_S};
    TEST_ASSERT_TRUE(ui_init(&p));
    ui_config_t c = cfg();
    c.idle_dim_s = 0;
    c.idle_off_s = 0;
    ui_set_config(&c);
    run(20);
    SWIPE_UP();
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings"));
    run(59000);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings"));
    run(2000); /* cards_timeout_s: nobody is choosing */
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
}

static void test_settings_closed_when_the_screen_goes_off(void)
{
    run(20);
    SWIPE_UP();
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings"));
    run(301000); /* idle off */
    TEST_ASSERT_TRUE(state().asleep);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_UP, BOARD_PRESS_SHORT)); /* only wakes */
    run(20);
    TEST_ASSERT_NULL(strstr(text(), "Settings"));
}

static void test_sleep_now(void)
{
    npipe_notif_t a = notif(1, PROTO_CATEGORY_SOCIAL, "Zalo", "A", "");
    npipe_notif_t b = notif(2, PROTO_CATEGORY_SOCIAL, "Viber", "B", "");
    ui_on_notification(&a);
    ui_on_notification(&b);
    run(5000);
    TEST_ASSERT_FALSE(ui_screen_off());
    TEST_ASSERT_EQUAL_UINT8(70, state().backlight);
    /* both keys pressed together: off at once, well before display.idle_off_s */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_UP_DOWN, BOARD_PRESS_SHORT));
    run(20);
    TEST_ASSERT_TRUE(ui_screen_off());
    TEST_ASSERT_TRUE(state().asleep);
    TEST_ASSERT_EQUAL_UINT8(0, state().backlight);
    run(60000);
    TEST_ASSERT_TRUE(state().asleep); /* stays off (no idle timer brings it back) */
    /* any key wakes it and does nothing else */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_DOWN, BOARD_PRESS_SHORT));
    run(20);
    TEST_ASSERT_FALSE(ui_screen_off());
    TEST_ASSERT_FALSE(state().asleep);
    TEST_ASSERT_EQUAL_UINT8(70, state().backlight);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Viber\n1/2\n"));
    TEST_ASSERT_EQUAL_UINT32(0, state().violations);

    /* the chord when the screen is already off only wakes it; the long chord too */
    ui_on_button(BOARD_BTN_UP_DOWN, BOARD_PRESS_SHORT);
    run(20);
    TEST_ASSERT_TRUE(ui_screen_off());
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_UP_DOWN, BOARD_PRESS_SHORT));
    run(20);
    TEST_ASSERT_FALSE(ui_screen_off());
    ui_on_button(BOARD_BTN_UP_DOWN, BOARD_PRESS_SHORT);
    run(20);
    TEST_ASSERT_EQUAL_INT(UI_ACTION_NONE, ui_on_button(BOARD_BTN_UP_DOWN, BOARD_PRESS_LONG));
    run(20);
    TEST_ASSERT_FALSE(ui_screen_off());
    /* the long chord (3 s) still toggles pairing mode while the screen is on */
    TEST_ASSERT_EQUAL_INT(UI_ACTION_PAIRING_TOGGLE, ui_on_button(BOARD_BTN_UP_DOWN, BOARD_PRESS_LONG));

    /* a notification wakes it, a touch too (the touch that wakes does nothing else) */
    ui_on_button(BOARD_BTN_UP_DOWN, BOARD_PRESS_SHORT);
    run(20);
    TEST_ASSERT_TRUE(ui_screen_off());
    npipe_notif_t c = notif(3, PROTO_CATEGORY_SOCIAL, "Line", "C", "");
    ui_on_notification(&c);
    run(20);
    TEST_ASSERT_FALSE(ui_screen_off());
    TEST_ASSERT_NOT_NULL(strstr(text(), "Line"));
    ui_on_button(BOARD_BTN_UP_DOWN, BOARD_PRESS_SHORT);
    run(20);
    drag(148, 60, 148, 200, 6); /* swipe down: only wakes, the card is not dismissed */
    TEST_ASSERT_FALSE(ui_screen_off());
    TEST_ASSERT_EQUAL_UINT32(0, n_actions);
}

static void test_sleep_now_with_idle_off_never(void)
{
    ui_config_t c = cfg();
    c.idle_off_s = 0; /* the key gesture sleeps anyway */
    ui_set_config(&c);
    run(1000);
    ui_on_button(BOARD_BTN_UP_DOWN, BOARD_PRESS_SHORT);
    run(20);
    TEST_ASSERT_TRUE(ui_screen_off());
    run(600000);
    TEST_ASSERT_TRUE(ui_screen_off());
}

static void test_brightness_and_never(void)
{
    ui_config_t c = cfg();
    c.brightness = 15; /* below the dim level: dimming does not brighten it */
    c.idle_off_s = 0;  /* never off */
    ui_set_config(&c);
    run(20);
    TEST_ASSERT_EQUAL_UINT8(15, state().backlight);
    run(600000);
    TEST_ASSERT_EQUAL_UINT8(15, state().backlight);
    TEST_ASSERT_FALSE(state().asleep);
}

static void test_rotation(void)
{
    run(20);
    ui_config_t c = cfg();
    const uint8_t want[4] = {0, 0, 2, 2}; /* the nearest of 0 and 180 degrees */
    for (uint8_t o = 1; o <= 4; o++) {
        c.orientation = o % 4;
        ui_set_config(&c);
        sim_lcd_clear_counters();
        run(20);
        TEST_ASSERT_EQUAL_UINT8(want[o % 4], state().rotation);
        TEST_ASSERT_TRUE(state().pixels >= FULL_FRAME); /* redrawn */
    }
}

/* PLAN D59: the LCD fonts draw text themselves and every registry emoji through the colour fallback. */
static void test_colour_emoji_fallback(void)
{
    const lv_font_t *fonts[] = {&ui_font_lcd_16, &ui_font_lcd_20, &ui_font_lcd_bold_20};
    lv_font_glyph_dsc_t g;
    for (size_t f = 0; f < sizeof(fonts) / sizeof(fonts[0]); f++) {
        TEST_ASSERT_TRUE(lv_font_get_glyph_dsc(fonts[f], &g, 0x1EC7 /* ệ */, 0));
        TEST_ASSERT_EQUAL_PTR(fonts[f], g.resolved_font);
        TEST_ASSERT_EQUAL(LV_FONT_GLYPH_FORMAT_A4, g.format);
        uint32_t n = 0;
        for (size_t r = 0; r < proto_emoji_ranges_count; r++) {
            for (uint32_t cp = proto_emoji_ranges[r].first; cp <= proto_emoji_ranges[r].last; cp++, n++) {
                TEST_ASSERT_TRUE(lv_font_get_glyph_dsc(fonts[f], &g, cp, 0));
                TEST_ASSERT_EQUAL_PTR(&ui_font_emoji_colour, g.resolved_font);
                TEST_ASSERT_EQUAL(LV_FONT_GLYPH_FORMAT_IMAGE, g.format);
                const lv_image_dsc_t *img = g.gid.src;
                TEST_ASSERT_EQUAL(LV_COLOR_FORMAT_RGB565A8, img->header.cf);
                TEST_ASSERT_TRUE(g.box_w > 0 && g.box_w <= 24 && g.box_h > 0 && g.box_h <= 20);
                /* LZ4 (PLAN D60): lv_image_compressed_t (method, compressed and decompressed size), then the block */
                TEST_ASSERT_TRUE(img->header.flags & LV_IMAGE_FLAGS_COMPRESSED);
                uint32_t head[3];
                memcpy(head, img->data, sizeof(head));
                TEST_ASSERT_EQUAL_UINT32(LV_IMAGE_COMPRESS_LZ4, head[0] & 0xFu);
                TEST_ASSERT_EQUAL_UINT32(img->data_size - 12u, head[1]);
                TEST_ASSERT_EQUAL_UINT32(3u * g.box_w * g.box_h, head[2]);
                TEST_ASSERT_TRUE(g.adv_w > g.box_w);
            }
        }
        TEST_ASSERT_EQUAL_UINT32(1378, n);
        TEST_ASSERT_FALSE(lv_font_get_glyph_dsc(&ui_font_emoji_colour, &g, 'A', 0));
    }
}

static void test_brightness_step(void)
{
    TEST_ASSERT_EQUAL_UINT8(80, ui_brightness_step(70, true));
    TEST_ASSERT_EQUAL_UINT8(60, ui_brightness_step(70, false));
    TEST_ASSERT_EQUAL_UINT8(100, ui_brightness_step(100, true));
    TEST_ASSERT_EQUAL_UINT8(10, ui_brightness_step(10, false)); /* never off from the keys */
    TEST_ASSERT_EQUAL_UINT8(10, ui_brightness_step(0, false));
    TEST_ASSERT_EQUAL_UINT8(80, ui_brightness_step(73, true)); /* 73 -> 70 -> 80 */
    TEST_ASSERT_EQUAL_UINT8(70, ui_brightness_step(76, false)); /* 76 -> 80 -> 70 */
}


/* ---------------------------------------------------------------- clock faces */

#define T_14_27_05 1791728825u /* Sunday 2026-10-11 14:27:05 UTC */

static ui_config_t face_cfg(void)
{
    ui_config_t c = cfg();
    c.tz_offset_min = 0;
    return c;
}

static uint16_t pixel(uint32_t x, uint32_t y) { return sim_lcd_frame()[y * 296u + x]; }

static void test_face_builtin_faces_draw_and_fall_back(void)
{
    ui_set_time(T_14_27_05);
    face_config(face_cfg(), "epager.big", NULL, 0);
    run(300);
    TEST_ASSERT_EQUAL_STRING("epager.big", ui_clock_face_shown());
    TEST_ASSERT_NOT_NULL(strstr(text(), "14:27\n"));
    TEST_ASSERT_NOT_NULL(strstr(text(), "Sunday, 11 Oct"));
    TEST_ASSERT_EQUAL_UINT16(0x10c4, pixel(2, 2)); /* #111827 as RGB565: the face's page, not the light Classic one */
    face_config(face_cfg(), "epager.night", NULL, 0);
    run(300);
    TEST_ASSERT_EQUAL_STRING("epager.night", ui_clock_face_shown());
    TEST_ASSERT_NOT_NULL(strstr(text(), "14:27\n")); /* the clock font as text */
    ui_status_t st = {.battery_pct = 63, .wifi_state = 0};
    ui_set_status(&st);
    run(300);
    TEST_ASSERT_NOT_NULL(strstr(text(), "63\n")); /* inside the battery ring */
    face_config(face_cfg(), "epager.nope", NULL, 0);
    run(300);
    TEST_ASSERT_EQUAL_STRING("", ui_clock_face_shown());
    TEST_ASSERT_EQUAL_UINT32(1, ui_clock_face_errors());
    TEST_ASSERT_NOT_EQUAL_UINT16(0x10c4, pixel(2, 2));
    TEST_ASSERT_EQUAL_UINT32(0, state().violations);
}

static void test_face_colour_option_and_another_screen(void)
{
    ui_set_time(T_14_27_05);
    face_config(face_cfg(), "epager.night", NULL, 0);
    run(300);
    const uint16_t page = pixel(2, 2);
    TEST_ASSERT_EQUAL_UINT16(0x0022, page); /* #020617 */
    static const uint8_t no_ring[] = {3, 0, 1, 0};
    face_config(face_cfg(), "epager.night", no_ring, sizeof(no_ring));
    ui_status_t st = {.battery_pct = 63};
    ui_set_status(&st);
    run(300);
    TEST_ASSERT_NULL(strstr(text(), "63\n")); /* the ring and its number are off */
    static uint8_t file[FACE_FILE_MAX];
    size_t n = vector_face("paper_min", file, sizeof(file)); /* made for 200x200 */
    install_face(&blob, "test.min", file, n);
    face_config(face_cfg(), "test.min", NULL, 0);
    run(300);
    TEST_ASSERT_EQUAL_STRING("", ui_clock_face_shown());
    TEST_ASSERT_EQUAL_UINT32(1, ui_clock_face_errors());
    n = vector_face("lcd_min", file, sizeof(file));
    install_face(&blob, "test.min", file, n);
    run(300);
    TEST_ASSERT_EQUAL_STRING("test.min", ui_clock_face_shown());
    TEST_ASSERT_EQUAL_UINT16(0x0000, pixel(2, 2)); /* its 10x10 black square */
    n = vector_face("lcd_heavy", file, sizeof(file));
    install_face(&blob, "test.heavy", file, n);
    face_config(face_cfg(), "test.heavy", NULL, 0);
    run(300);
    TEST_ASSERT_EQUAL_STRING("", ui_clock_face_shown());
    TEST_ASSERT_EQUAL_UINT32(2, ui_clock_face_errors());
}

static void test_face_badge_and_update_mark(void)
{
    ui_set_time(T_14_27_05);
    face_config(face_cfg(), "epager.big", NULL, 0);
    npipe_notif_t a = notif(1, PROTO_CATEGORY_SOCIAL, "Zalo", "First", "a"), b = notif(2, PROTO_CATEGORY_EMAIL, "Mail", "Second", "b");
    ui_on_notification(&a);
    ui_on_notification(&b);
    run(2100);
    drag(148, 200, 148, 60, 6); /* swipe up: the clock, the notifications stay */
    run(300);
    TEST_ASSERT_NOT_NULL(strstr(text(), "\n2\n")); /* the badge, on a built-in face */
    ui_update_t u = {.state = UI_UPD_AVAILABLE, .mark = true};
    snprintf(u.version, sizeof(u.version), "0.3.0");
    ui_set_update(&u);
    run(300);
    TEST_ASSERT_NOT_NULL(strstr(text(), "!\n")); /* the firmware's mark: the face does not draw update_available */
    static uint8_t file[FACE_FILE_MAX];
    const size_t n = vector_face("lcd_min", file, sizeof(file));
    install_face(&blob, "test.min", file, n);
    face_config(face_cfg(), "test.min", NULL, 0);
    run(300);
    TEST_ASSERT_EQUAL_STRING("test.min", ui_clock_face_shown());
    TEST_ASSERT_NULL(strstr(text(), "\n2\n")); /* never on an installed face */
    TEST_ASSERT_NOT_NULL(strstr(text(), "!\n"));
}

static void test_face_other_screens_cover_it(void)
{
    ui_set_time(T_14_27_05);
    face_config(face_cfg(), "epager.big", NULL, 0);
    run(300);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Sunday, 11 Oct")); /* the face's date */
    drag(148, 200, 148, 60, 6); /* swipe up on the clock: the settings (D99) */
    run(300);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Settings"));
    TEST_ASSERT_NULL(strstr(text(), "Sunday, 11 Oct")); /* the face is under the settings, not on top */
    ui_set_power(false);
    run(300);
    TEST_ASSERT_NOT_NULL(strstr(text(), "Powered off"));
    TEST_ASSERT_NULL(strstr(text(), "Sunday, 11 Oct"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_frame_then_backlight);
    RUN_TEST(test_only_changes_are_sent);
    RUN_TEST(test_card_and_keys);
    RUN_TEST(test_card_masks_numbers_when_asked);
    RUN_TEST(test_colour_app_icon);
    RUN_TEST(test_swipes_and_dismiss);
    RUN_TEST(test_hide_cards_and_the_badge);
    RUN_TEST(test_cards_time_out);
    RUN_TEST(test_tap_shows_the_whole_card);
    RUN_TEST(test_dim_off_and_wake);
    RUN_TEST(test_waking_touch_does_nothing_else);
    RUN_TEST(test_settings_open_pages_and_back);
    RUN_TEST(test_settings_tap_changes_a_value);
    RUN_TEST(test_settings_clock_face_pages);
    RUN_TEST(test_settings_pairing_and_pages);
    RUN_TEST(test_settings_closes);
    RUN_TEST(test_settings_with_wifi);
    RUN_TEST(test_update_page_and_rows);
    RUN_TEST(test_update_mark_and_prompt);
    RUN_TEST(test_update_prompt_waits_for_an_awake_screen);
    RUN_TEST(test_settings_close_by_idle);
    RUN_TEST(test_settings_closed_when_the_screen_goes_off);
    RUN_TEST(test_sleep_now);
    RUN_TEST(test_sleep_now_with_idle_off_never);
    RUN_TEST(test_brightness_and_never);
    RUN_TEST(test_brightness_step);
    RUN_TEST(test_rotation);
    RUN_TEST(test_colour_emoji_fallback);
    RUN_TEST(test_weather_row);
    RUN_TEST(test_wifi_status_icon);
    RUN_TEST(test_pairing_screen);
    RUN_TEST(test_pairing_passkey);
    RUN_TEST(test_wifi_setup_screen);
    RUN_TEST(test_lvgl_pool_headroom);
    RUN_TEST(test_face_builtin_faces_draw_and_fall_back);
    RUN_TEST(test_face_colour_option_and_another_screen);
    RUN_TEST(test_face_badge_and_update_mark);
    RUN_TEST(test_face_other_screens_cover_it);
    return UNITY_END();
}
