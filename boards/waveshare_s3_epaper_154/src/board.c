/*
 * Waveshare ESP32-S3-ePaper-1.54 V2: the board_api implementation (BoardProfile). The display is in
 * ws154_epd.c, the RTC and the temperature/humidity sensor in ws154_i2c.c, every pin in ws154_pins.h. Nothing here has run on hardware yet (TODO(hw-verify)
 * items are listed in docs/hw-checklist.md).
 */
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "epager/board_api.h"
#include "epager/board_util.h"
#include "epager/proto_gen.h"
#include "ws154_audio.h"
#include "ws154_epd.h"
#include "ws154_i2c.h"
#include "ws154_pins.h"

#define TAG "ws154"

#define BOARD_ID "waveshare_s3_epaper_154_v2"
#define FLASH_BYTES (8u * 1024u * 1024u)
#define LOW_BATTERY_OTA_PCT 30 /* PLAN D11, approved; the battery type is still unknown */

/* ---------------------------------------------------------------- init, rails */

static board_profile_t profile;

/* RTC and sensor: in the profile (and its features) only if they answered at boot. */
static void init_i2c_chips(void)
{
    if (!ws154_i2c_init()) {
        return;
    }
    if (ws154_rtc_present()) {
        profile.rtc_get = ws154_rtc_get;
        profile.rtc_set = ws154_rtc_set;
        profile.features |= PROTO_FEATURES_RTC;
    }
    if (ws154_climate_present()) {
        profile.read_climate = ws154_read_climate;
        profile.features |= PROTO_FEATURES_CLIMATE;
    }
}

static bool board_init(void *ctx)
{
    (void)ctx;
    /* The latch first: on battery the board is only powered while the PWR key is held or this pin is
     * high. The level is set before the pin becomes an output, so it never glitches low. */
    gpio_set_level(WS154_BAT_LATCH, 1);
    gpio_set_level(WS154_EPD_PWR, 1);   /* panel rail off (active low) */
    gpio_set_level(WS154_AUDIO_PWR, 1); /* audio rail off (active low); the firmware never turns it on */
    gpio_config_t out = {.pin_bit_mask = (1ULL << WS154_BAT_LATCH) | (1ULL << WS154_EPD_PWR) | (1ULL << WS154_AUDIO_PWR),
                         .mode = GPIO_MODE_OUTPUT};
    bool ok = gpio_config(&out) == ESP_OK;
    /* Light sleep (PLAN D17, D87) isolates every pin whose sleep configuration is left alone: the latch would go
     * low and the battery would be disconnected, switching the board off the moment it sleeps without USB. The
     * latch and the two rails keep driving their levels while asleep. */
    for (gpio_num_t pin = 0; pin < GPIO_NUM_MAX; pin++) {
        if (out.pin_bit_mask & (1ULL << pin)) {
            ok &= gpio_sleep_sel_dis(pin) == ESP_OK;
        }
    }
    init_i2c_chips();
#if CONFIG_EPAGER_BOOT_SOUND
    ok &= ws154_audio_init_pins();
#endif
    return ok;
}

static void set_rail(void *ctx, board_rail_t rail, bool on)
{
    (void)ctx;
    switch (rail) {
    case BOARD_RAIL_DISPLAY:
        gpio_set_level(WS154_EPD_PWR, on ? 0 : 1); /* the display driver owns this rail in normal use */
        break;
    case BOARD_RAIL_AUDIO:
        ws154_i2c_hold_rail(on); /* held on, or back to the rail around each I2C use */
        break;
    case BOARD_RAIL_LATCH:
        gpio_set_level(WS154_BAT_LATCH, on ? 1 : 0); /* off = power off when running on battery only */
        break;
    }
}

/* PLAN D88. On battery the latch going low cuts the supply, but the PWR key itself also closes the supply path
 * (through D9), so the board only dies once the key is let go: wait for that first. On USB the chip stays
 * powered: deep-sleep with the PWR key as the wake-up (the reset that follows is a normal power-on, board_init
 * takes the latch again). TODO(hw-verify): checklist 70. */
static void power_off(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "power off");
    gpio_set_level(WS154_BAT_LATCH, 0);
    gpio_set_level(WS154_EPD_PWR, 1);
    gpio_set_level(WS154_AUDIO_PWR, 1);
    for (int up = 0; up < 5;) { /* the key high for 50 ms; on battery the board is gone before the loop ends */
        up = gpio_get_level(WS154_BTN_PWR) ? up + 1 : 0;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "still powered (USB): deep sleep until the PWR key");
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    gpio_sleep_set_pull_mode(WS154_BTN_PWR, GPIO_PULLUP_ONLY);
    if (esp_sleep_enable_ext1_wakeup_io(1ULL << WS154_BTN_PWR, ESP_EXT1_WAKEUP_ANY_LOW) != ESP_OK) {
        ESP_LOGE(TAG, "key wake-up not available");
        return;
    }
    esp_deep_sleep_start();
}

/* ---------------------------------------------------------------- buttons */

#define BTN_POLL_US 10000
#define BTN_DEBOUNCE_SAMPLES 3
#define BTN_LONG_MS 1500 /* TODO(hw-verify): feel; settings: BOOT long = select, PWR long = back */
#define PWR_HOLD_MS 5000 /* PWR held this long = power off (PLAN D96); TODO(hw-verify): feel */
#define CHORD_MS 1500    /* both keys held = pairing mode (PLAN D75); TODO(hw-verify): feel */

typedef struct {
    gpio_num_t pin;
    board_button_t id;
    uint32_t hold_ms; /* 0: no HOLD */
    board_btn_state_t st;
} button_t;

static struct {
    board_button_cb cb;
    void *user;
    esp_timer_handle_t timer;
    button_t b[2];
    board_chord_state_t chord;
} btn = {.b = {{.pin = WS154_BTN_BOOT, .id = BOARD_BTN_BOOT}, {.pin = WS154_BTN_PWR, .id = BOARD_BTN_PWR, .hold_ms = PWR_HOLD_MS}}};

/* Runs in the esp_timer task: the callback must only queue work for the UI task. */
static void button_poll(void *arg)
{
    (void)arg;
    const bool down[2] = {gpio_get_level(btn.b[0].pin) == 0, gpio_get_level(btn.b[1].pin) == 0};
    const bool was_chord = btn.chord.active;
    /* only the long chord is used: a tap window that can never match turns the SHORT report off */
    const board_btn_ev_t chord = board_chord_step(&btn.chord, down[0], down[1], BTN_POLL_US / 1000, CHORD_MS, 1, 0);
    if (chord == BOARD_BTN_EV_LONG && btn.cb != NULL) {
        btn.cb(btn.user, BOARD_BTN_BOOT_PWR, BOARD_PRESS_LONG);
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
        board_btn_ev_t ev = board_btn_step_hold(&b->st, down[i], BTN_POLL_US / 1000, BTN_LONG_MS, b->hold_ms,
                                                BTN_DEBOUNCE_SAMPLES);
        if (ev != BOARD_BTN_EV_NONE && btn.cb != NULL) {
            btn.cb(btn.user, b->id,
                   ev == BOARD_BTN_EV_HOLD ? BOARD_PRESS_HOLD : ev == BOARD_BTN_EV_LONG ? BOARD_PRESS_LONG : BOARD_PRESS_SHORT);
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
    gpio_config_t in = {.pin_bit_mask = (1ULL << WS154_BTN_BOOT) | (1ULL << WS154_BTN_PWR), .mode = GPIO_MODE_INPUT};
    esp_timer_create_args_t args = {.callback = button_poll, .name = "buttons"};
    if (gpio_config(&in) != ESP_OK || esp_timer_create(&args, &btn.timer) != ESP_OK ||
        esp_timer_start_periodic(btn.timer, BTN_POLL_US) != ESP_OK) {
        ESP_LOGE(TAG, "button polling not started");
        btn.timer = NULL;
    }
}

/*
 * Light sleep (PLAN D17, D82): the 10 ms polling timer would wake the chip a hundred times a second, so while
 * the app is idle it is stopped and a low level on either key wakes the chip instead (any GPIO can wake light
 * sleep). The interrupt tells the app, which turns the polling back on; the key is still down, so the press is
 * timed from the start and a long press works. With a key already down the arming is refused: the polling
 * must time that press. TODO(hw-verify): checklist 65.
 */
static struct {
    void (*wake_isr)(void *user);
    void *wake_user;
    bool isr_ready;
} slp;

static void key_wake_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(WS154_BTN_BOOT);
    gpio_intr_disable(WS154_BTN_PWR);
    if (slp.wake_isr != NULL) {
        slp.wake_isr(slp.wake_user);
    }
}

static bool set_sleep(void *ctx, bool sleeping, void (*wake_isr)(void *user), void *user)
{
    (void)ctx;
    if (btn.timer == NULL) {
        return true;
    }
    const gpio_num_t pins[2] = {WS154_BTN_BOOT, WS154_BTN_PWR};
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
    if (gpio_get_level(pins[0]) == 0 || gpio_get_level(pins[1]) == 0) {
        return false;
    }
    if (!slp.isr_ready) {
        const esp_err_t e = gpio_install_isr_service(0);
        if ((e != ESP_OK && e != ESP_ERR_INVALID_STATE) || gpio_isr_handler_add(pins[0], key_wake_isr, NULL) != ESP_OK ||
            gpio_isr_handler_add(pins[1], key_wake_isr, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "key wake-up not available");
            return true;
        }
        slp.isr_ready = true;
    }
    esp_timer_stop(btn.timer);
    slp.wake_isr = wake_isr;
    slp.wake_user = user;
    btn.b[0].st = (board_btn_state_t){0};
    btn.b[1].st = (board_btn_state_t){0};
    btn.chord = (board_chord_state_t){0};
    for (size_t i = 0; i < 2; i++) {
        gpio_wakeup_enable(pins[i], GPIO_INTR_LOW_LEVEL); /* keys are active low; also sets the interrupt type */
        gpio_intr_enable(pins[i]);                        /* a key that went down just now fires at once */
    }
    esp_sleep_enable_gpio_wakeup();
    return true;
}

/* ---------------------------------------------------------------- battery */

/*
 * VBAT / 2 on ADC1 channel 3 (GPIO4, ws154_pins.h), 12 dB attenuation, curve-fitting calibration
 * when the chip's eFuses carry the calibration data.
 * TODO(hw-verify): the real divider ratio and the ADC accuracy (compare with a multimeter).
 * TODO(verify): the charger (ETA6098) termination voltage and the battery chemistry/capacity:
 * board_lipo_pct() is the usual single-cell LiPo discharge curve, a stand-in until the battery is known;
 * its top is stretched to where this board's charger stops (WS154_BAT_FULL_MV, PLAN D104).
 */
static struct {
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t cali;
    adc_channel_t chan;
    bool ready, failed;
} adc;

/* The divider is 2 x 200 K: a 100 K source for the ADC's sampling capacitor, so single readings come out low and
 * scattered. Throw away the first ones, take many, and average the middle half (PLAN D84). */
#define ADC_SAMPLES 40
#define ADC_SETTLE 4
#define NO_BATTERY_MV 2500 /* below this the divider is floating: no cell connected */

static bool adc_setup(void)
{
    if (adc.ready) {
        return true;
    }
    if (adc.failed) {
        return false;
    }
    adc_unit_t unit;
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1};
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_io_to_channel(WS154_BAT_ADC_GPIO, &unit, &adc.chan) != ESP_OK || unit != ADC_UNIT_1 ||
        adc_oneshot_new_unit(&ucfg, &adc.unit) != ESP_OK ||
        adc_oneshot_config_channel(adc.unit, adc.chan, &ccfg) != ESP_OK) {
        adc.failed = true;
        return false;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal = {.unit_id = ADC_UNIT_1, .chan = adc.chan, .atten = ADC_ATTEN_DB_12,
                                           .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cal, &adc.cali) != ESP_OK) {
        adc.cali = NULL;
        ESP_LOGW(TAG, "no ADC calibration data: battery level is approximate");
    }
#endif
    adc.ready = true;
    return true;
}

static int battery_mv(int *spread_mv)
{
    if (!adc_setup()) {
        return -1;
    }
    int v[ADC_SAMPLES];
    for (int i = 0; i < ADC_SETTLE + ADC_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(adc.unit, adc.chan, &raw) != ESP_OK) {
            return -1;
        }
        if (i >= ADC_SETTLE) {
            int j = i - ADC_SETTLE;
            for (; j > 0 && v[j - 1] > raw; j--) { /* insertion sort */
                v[j] = v[j - 1];
            }
            v[j] = raw;
        }
    }
    int sum = 0;
    for (int i = ADC_SAMPLES / 4; i < ADC_SAMPLES - ADC_SAMPLES / 4; i++) {
        sum += v[i];
    }
    int raw = sum / (ADC_SAMPLES / 2), mv;
    if (adc.cali == NULL || adc_cali_raw_to_voltage(adc.cali, raw, &mv) != ESP_OK) {
        mv = raw * 3100 / 4095; /* rough full scale of 12 dB attenuation */
    }
    if (spread_mv != NULL) {
        *spread_mv = (v[ADC_SAMPLES - 1] - v[0]) * 3100 / 4095 * WS154_BAT_DIVIDER;
    }
    return mv * WS154_BAT_DIVIDER;
}

static board_batt_filter_t batt_filter = {.shown_pct = -1};
static board_battery_diag_t batt_diag; /* the last reading, for battery_diag; raw and smoothed are millivolts */
static bool batt_diag_valid;

static bool read_battery(void *ctx, uint8_t *pct, uint8_t *external)
{
    (void)ctx;
    /* No GPIO senses VBUS or the charger state (ws154_pins.h): external power is unknown, so the OTA
     * battery gate uses the charge level alone. */
    *external = PROTO_EXTERNAL_POWER_UNKNOWN;
    int spread;
    const int raw_mv = battery_mv(&spread);
    if (raw_mv < NO_BATTERY_MV) {
        return false;
    }
    const int mv = board_batt_filter_mv(&batt_filter, raw_mv, spread);
    *pct = board_batt_hold_pct(&batt_filter, board_lipo_pct_full(board_batt_level_mv(&batt_filter, mv), WS154_BAT_FULL_MV));
    /* a guess from the voltage signature, not a sensor (PLAN D85): YES lets the clock screen show "USB" */
    *external = batt_filter.charging ? PROTO_EXTERNAL_POWER_YES : PROTO_EXTERNAL_POWER_UNKNOWN;
    batt_diag = (board_battery_diag_t){.raw = raw_mv, .smoothed = mv, .pct = *pct, .charging = batt_filter.charging};
    batt_diag_valid = true;
    ESP_LOGI(TAG, "battery: %d mV read (samples spread %d mV), %d mV steadied, %u %%%s", raw_mv, spread, mv,
             (unsigned)*pct, batt_filter.charging ? ", charging" : "");
    return true;
}

static bool battery_diag(void *ctx, board_battery_diag_t *out)
{
    (void)ctx;
    *out = batt_diag;
    return batt_diag_valid;
}

/* ---------------------------------------------------------------- profile */

static board_profile_t profile = {
    .board_id = BOARD_ID,
    .flash_size = FLASH_BYTES,
    .features = PROTO_FEATURES_BATTERY | PROTO_FEATURES_BUTTONS, /* hardware only; software features come from the variant */
    .low_battery_ota_pct = LOW_BATTERY_OTA_PCT,
    .read_battery = read_battery,
    .battery_diag = battery_diag,
    .set_button_handler = set_button_handler,
    .set_sleep = set_sleep,
    .set_rail = set_rail,
    .power_off = power_off,
#if CONFIG_EPAGER_BOOT_SOUND
    .play_audio = ws154_audio_play,
#endif
    /* rtc_get, rtc_set, read_climate: set by board_init when the chips answer */
    .init = board_init,
};

const board_profile_t *board_get(void)
{
    profile.display = ws154_epd_get();
    return &profile;
}
