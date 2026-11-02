/*
 * ostb-xiaozhi-3st (LCD board): the board_api implementation. The LCD is in xz3_lcd.c, the I2C
 * bus and touch in xz3_touch.c, every pin in xz3_pins.h. There is no schematic: pins and settings
 * come from the stock firmware's code (docs/ostb-xiaozhi-3st.md). Nothing here has run on the board
 * yet: TODO(hw-verify) items are in docs/hw-checklist.md (21-28).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include "epager/board_api.h"
#include "epager/board_util.h"
#include "epager/proto_gen.h"
#include "xz3_lcd.h"
#include "xz3_pins.h"
#include "xz3_touch.h"

#define TAG "xz3"

#define BOARD_ID "ostb_xiaozhi_3st"
#define FLASH_BYTES (16u * 1024u * 1024u)
#define LOW_BATTERY_OTA_PCT 30 /* the same rule as the Paper (PLAN D11); battery capacity unknown */

static board_profile_t profile;

/* ---------------------------------------------------------------- init, rails */

static bool board_init(void *ctx)
{
    (void)ctx;
    /* Quiet and dark first: the speaker amplifier off (a floating enable can hiss), the backlight off
     * until the LCD driver has a black frame on the panel. Levels before direction: no glitch. */
    gpio_set_level(XZ3_AMP_EN, 0);
    gpio_set_level(XZ3_LCD_BL, 0);
    gpio_config_t out = {.pin_bit_mask = (1ULL << XZ3_AMP_EN) | (1ULL << XZ3_LCD_BL), .mode = GPIO_MODE_OUTPUT};
    gpio_config_t chg = {.pin_bit_mask = 1ULL << XZ3_CHG_STAT, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE};
    bool ok = gpio_config(&out) == ESP_OK && gpio_config(&chg) == ESP_OK;
    if (xz3_i2c_init()) {
        profile.read_touch = xz3_touch_read;
        profile.features |= PROTO_FEATURES_TOUCH;
    }
    return ok;
}

static void set_rail(void *ctx, board_rail_t rail, bool on)
{
    (void)ctx;
    if (rail == BOARD_RAIL_AUDIO) {
        gpio_set_level(XZ3_AMP_EN, on ? 1 : 0); /* the amplifier only; the codecs are never configured */
    }
    /* No display rail (the backlight is display->set_backlight) and no power latch: the power key
     * switches the board in hardware. */
}

/* ---------------------------------------------------------------- keys */

#define BTN_POLL_US 10000
#define BTN_DEBOUNCE_SAMPLES 3
#define BTN_LONG_MS 1000 /* felt right on the board (checklist item 26) */
#define CHORD_MS 3000    /* both keys held: pairing mode (PLAN 7c) */
#define CHORD_TAP_MIN_MS 150  /* both keys pressed and released within 150 ms..1.5 s: sleep now */
#define CHORD_TAP_MAX_MS 1500

typedef struct {
    gpio_num_t pin;
    board_button_t id;
    board_btn_state_t st;
} button_t;

static struct {
    board_button_cb cb;
    void *user;
    esp_timer_handle_t timer;
    button_t b[2];
    board_chord_state_t chord;
    void (*wake_isr)(void *user);
    void *wake_user;
    bool isr_ready;
} btn = {.b = {{.pin = XZ3_KEY_UP, .id = BOARD_BTN_UP}, {.pin = XZ3_KEY_DOWN, .id = BOARD_BTN_DOWN}}};

/* Runs in the esp_timer task: the callback must only queue work for the UI task. */
static void button_poll(void *arg)
{
    (void)arg;
    const bool down[2] = {gpio_get_level(btn.b[0].pin) == 0, gpio_get_level(btn.b[1].pin) == 0};
    const bool was_chord = btn.chord.active;
    const board_btn_ev_t chord = board_chord_step(&btn.chord, down[0], down[1], BTN_POLL_US / 1000, CHORD_MS,
                                                  CHORD_TAP_MIN_MS, CHORD_TAP_MAX_MS);
    if (chord != BOARD_BTN_EV_NONE && btn.cb != NULL) {
        btn.cb(btn.user, BOARD_BTN_UP_DOWN, chord == BOARD_BTN_EV_LONG ? BOARD_PRESS_LONG : BOARD_PRESS_SHORT);
    }
    if (btn.chord.active) {
        if (!was_chord) { /* a key already down must not report a press when the chord ends */
            btn.b[0].st = (board_btn_state_t){0};
            btn.b[1].st = (board_btn_state_t){0};
        }
        return;
    }
    for (size_t i = 0; i < 2; i++) {
        button_t *b = &btn.b[i];
        board_btn_ev_t ev = board_btn_step(&b->st, down[i], BTN_POLL_US / 1000, BTN_LONG_MS, BTN_DEBOUNCE_SAMPLES);
        if (ev != BOARD_BTN_EV_NONE && btn.cb != NULL) {
            btn.cb(btn.user, b->id, ev == BOARD_BTN_EV_LONG ? BOARD_PRESS_LONG : BOARD_PRESS_SHORT);
        }
    }
}

static void set_button_handler(void *ctx, board_button_cb cb, void *user)
{
    (void)ctx;
    btn.cb = cb;
    btn.user = user;
    if (btn.timer != NULL || cb == NULL) {
        return;
    }
    /* Active low (the stock firmware's Button(gpio, active_high = false)); the internal pull-ups are on
     * in case the board has none. */
    gpio_config_t in = {.pin_bit_mask = (1ULL << XZ3_KEY_UP) | (1ULL << XZ3_KEY_DOWN), .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_ENABLE};
    esp_timer_create_args_t args = {.callback = button_poll, .name = "buttons"};
    if (gpio_config(&in) != ESP_OK || esp_timer_create(&args, &btn.timer) != ESP_OK ||
        esp_timer_start_periodic(btn.timer, BTN_POLL_US) != ESP_OK) {
        ESP_LOGE(TAG, "key polling not started");
        btn.timer = NULL;
    }
}

/*
 * Light sleep (PLAN D17): the 10 ms polling timer would wake the chip a hundred times a second, so while
 * the screen is off it is stopped and a low level on either key wakes the chip instead (GPIO39/40 are not
 * RTC pins, which is fine for light sleep: the digital GPIO wake-up works on any pin). The interrupt tells
 * the app, which turns the polling back on; the press is then debounced as usual and reaches the UI, which
 * wakes the screen on it. TODO(hw-verify): checklist 14 and 30.
 */
static void key_wake_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(XZ3_KEY_UP);
    gpio_intr_disable(XZ3_KEY_DOWN);
    if (btn.wake_isr != NULL) {
        btn.wake_isr(btn.wake_user);
    }
}

static bool set_sleep(void *ctx, bool sleeping, void (*wake_isr)(void *user), void *user)
{
    (void)ctx;
    if (btn.timer == NULL) {
        return true;
    }
    const gpio_num_t pins[2] = {XZ3_KEY_UP, XZ3_KEY_DOWN};
    if (!sleeping) {
        for (size_t i = 0; i < 2; i++) {
            gpio_intr_disable(pins[i]);
            gpio_wakeup_disable(pins[i]);
        }
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
        if (!esp_timer_is_active(btn.timer)) {
            esp_timer_start_periodic(btn.timer, BTN_POLL_US);
        }
        return true;
    }
    if (!btn.isr_ready) {
        const esp_err_t e = gpio_install_isr_service(0);
        if ((e != ESP_OK && e != ESP_ERR_INVALID_STATE) || gpio_isr_handler_add(pins[0], key_wake_isr, NULL) != ESP_OK ||
            gpio_isr_handler_add(pins[1], key_wake_isr, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "key wake-up not available");
            return true;
        }
        btn.isr_ready = true;
    }
    esp_timer_stop(btn.timer);
    btn.wake_isr = wake_isr;
    btn.wake_user = user;
    btn.b[0].st = (board_btn_state_t){0};
    btn.b[1].st = (board_btn_state_t){0};
    btn.chord = (board_chord_state_t){0};
    for (size_t i = 0; i < 2; i++) {
        gpio_wakeup_enable(pins[i], GPIO_INTR_LOW_LEVEL); /* keys are active low; also sets the interrupt type */
        gpio_intr_enable(pins[i]);                        /* a key already down fires at once */
    }
    esp_sleep_enable_gpio_wakeup();
    return true;
}

/* ---------------------------------------------------------------- battery */

/*
 * GPIO17 = ADC2 channel 6, 12 dB attenuation, read raw (uncalibrated) and mapped through the stock
 * firmware's tables, so the level matches what the stock firmware showed. The divider and the cell
 * are unknown (docs/ostb-xiaozhi-3st.md). TODO(hw-verify): checklist item 27.
 */
static const uint16_t RAW_ON_BATTERY[] = {1890, 1950, 2010, 2070, 2130, 2190, 2250, 2310, 2370, 2430, 2460, 2489};
static const uint16_t RAW_CHARGING[] = {1890, 2050, 2110, 2160, 2220, 2275, 2330, 2380, 2420, 2465, 2490, 2510};
static const uint8_t LEVELS[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 100};
#define N_LEVELS (sizeof(LEVELS) / sizeof(LEVELS[0]))
#define ADC_SAMPLES 16

static struct {
    adc_oneshot_unit_handle_t unit;
    adc_channel_t chan;
    bool ready, failed;
    int smooth;                /* raw reading, smoothed like the stock firmware: 0.8 old + 0.2 new; 0 = none yet */
    int last_pct;
    bool last_charging;
    board_battery_diag_t diag; /* the last reading, for battery_diag */
} adc = {.last_pct = -1};

static bool adc_setup(void)
{
    if (adc.ready || adc.failed) {
        return adc.ready;
    }
    adc_unit_t unit;
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_2};
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    if (adc_oneshot_io_to_channel(XZ3_BAT_ADC_GPIO, &unit, &adc.chan) != ESP_OK || unit != ADC_UNIT_2 ||
        adc_oneshot_new_unit(&ucfg, &adc.unit) != ESP_OK ||
        adc_oneshot_config_channel(adc.unit, adc.chan, &ccfg) != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC setup failed");
        adc.failed = true;
        return false;
    }
    adc.ready = true;
    return true;
}

static bool read_battery(void *ctx, uint8_t *pct, uint8_t *external)
{
    (void)ctx;
    /* The charger's status line is low while it charges (seen on the board, item 27). High means either
     * no cable or a full cell on the cable, so only "charging" tells external power for sure. */
    const bool charging = gpio_get_level(XZ3_CHG_STAT) == 0;
    *external = charging ? PROTO_EXTERNAL_POWER_YES : PROTO_EXTERNAL_POWER_UNKNOWN;
    if (!adc_setup()) {
        return false;
    }
    int sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(adc.unit, adc.chan, &raw) != ESP_OK) {
            return false;
        }
        sum += raw;
    }
    const int raw = sum / ADC_SAMPLES;
    adc.smooth = adc.smooth == 0 ? raw : (adc.smooth * 4 + raw + 2) / 5;
    *pct = board_table_pct(adc.smooth, charging ? RAW_CHARGING : RAW_ON_BATTERY, LEVELS, N_LEVELS);
    adc.diag = (board_battery_diag_t){.raw = raw, .smoothed = adc.smooth, .pct = *pct, .charging = charging};
    /* Log a real change only: one level step flickers at a table point (seen on the board). */
    if (adc.last_pct < 0 || abs(*pct - adc.last_pct) >= 2 || charging != adc.last_charging) {
        ESP_LOGI(TAG, "battery: raw %d (smoothed %d), %u %%, %s", raw, adc.smooth, *pct,
                 charging ? "charging" : "not charging");
        adc.last_pct = *pct;
        adc.last_charging = charging;
    }
    return true;
}

static bool battery_diag(void *ctx, board_battery_diag_t *out)
{
    (void)ctx;
    *out = adc.diag;
    return adc.smooth != 0;
}

/* ---------------------------------------------------------------- profile */

static board_profile_t profile = {
    .board_id = BOARD_ID,
    .flash_size = FLASH_BYTES,
    .features = PROTO_FEATURES_BATTERY | PROTO_FEATURES_BUTTONS, /* touch added by board_init when it answers */
    .low_battery_ota_pct = LOW_BATTERY_OTA_PCT,
    .read_battery = read_battery,
    .battery_diag = battery_diag,
    .set_button_handler = set_button_handler,
    .set_sleep = set_sleep,
    .set_rail = set_rail,
    /* no RTC, no climate sensor; read_touch set by board_init */
    .init = board_init,
};

const board_profile_t *board_get(void)
{
    profile.display = xz3_lcd_get();
    return &profile;
}
