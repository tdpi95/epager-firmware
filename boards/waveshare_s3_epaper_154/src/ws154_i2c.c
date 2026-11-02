/*
 * I2C bus, PCF85063A RTC and SHTC3 sensor of the Waveshare ESP32-S3-ePaper-1.54 V2. Written from the
 * NXP PCF85063A datasheet Rev. 7.3 and the Sensirion SHTC3 datasheet v4 (Dec 2022); the register
 * and read-out formats are host-tested in board_util.h. Nothing here has run on the board yet:
 * TODO(hw-verify) items are in docs/hw-checklist.md (16-19).
 */
#include "ws154_i2c.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "epager/board_util.h"
#include "ws154_pins.h"

#define TAG "ws154_i2c"

/* 100 kHz: within both chips' limits (PCF85063A 400 kHz, SHTC3 1 MHz); the bus is short and the
 * transfers are a few bytes a minute, so speed buys nothing. External 4.7 K pull-ups (R20, R70). */
#define I2C_HZ 100000
#define I2C_TIMEOUT_MS 50
#define PCF85063_ADDR 0x51
#define SHTC3_ADDR 0x70

/* PCF85063A registers (datasheet table 4) */
#define PCF_CONTROL_1 0x00
#define PCF_CONTROL_2 0x01
#define PCF_SECONDS 0x04
#define PCF_C1_STOP 0x20
#define PCF_C1_12_24 0x02  /* 1 = 12-hour mode; we use 24-hour */
#define PCF_C2_CLKOUT_OFF 0x07 /* COF = 111; also AIE = MI = HMI = 0 (no interrupts) and clears AF, TF */

/* SHTC3 commands (datasheet tables 9-14), sent MSB first */
#define SHTC3_WAKEUP 0x3517
#define SHTC3_SLEEP 0xB098
#define SHTC3_READ_ID 0xEFC8
#define SHTC3_MEASURE_T_FIRST 0x7866 /* normal mode, no clock stretching: poll for the result */
#define SHTC3_ID_MASK 0x083F
#define SHTC3_ID 0x0807
#define SHTC3_WAKE_US 240     /* t_PU / t_SR max (table 5) */
#define SHTC3_MEASURE_MS 13   /* 12.1 ms max in normal mode */
#define SHTC3_POLLS 5

/* The ES8311 codec shares this bus and is powered from the audio rail (PA_EN, Q6): unpowered it clamps
 * SDA and SCL low (measured 2026-10-15, hw-checklist 16). So the rail is on only while we talk to a
 * chip (PLAN D73). TODO(hw-verify): the 10 ms settle was never tried below 20 ms. */
#define RAIL_SETTLE_MS 10

/* The first RTC transaction after a phone connects fails on every boot with "I2C hardware timeout detected"
 * (boot logs 2026-10-17); the driver resets its state machine for the next call and the clock is found correct
 * at the next boot, so a bounded retry of the idempotent RTC accesses is enough. The cause is not known yet:
 * the first retry logs the bus levels and the time since the rail came on (hw-checklist 74). */
#define I2C_TRIES 3
#define RETRY_PAUSE_MS 5

static struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t rtc, sht, codec;
    uint8_t control_1;
    bool rtc_valid_mode; /* false after we switched it out of 12-hour mode: its time means nothing */
    bool warned, retry_logged;
    int64_t rail_on_us; /* esp_timer time of the last rail-on, for the retry log */
} i2c = {.rtc_valid_mode = true};

/* Block at least ms milliseconds (the tick is 10 ms: pdMS_TO_TICKS alone could wait less). */
static void delay_at_least_ms(uint32_t ms)
{
    vTaskDelay((TickType_t)((ms * configTICK_RATE_HZ + 999u) / 1000u + 1u));
}

static bool rail_held; /* A/B drain test (PLAN D106): the rail stays on between uses */

void ws154_i2c_hold_rail(bool hold)
{
    rail_held = hold;
    gpio_set_level(WS154_AUDIO_PWR, hold ? 0 : 1);
}

static void bus_power(bool on)
{
    if (rail_held) {
        return;
    }
    gpio_set_level(WS154_AUDIO_PWR, on ? 0 : 1); /* active low (PMOS) */
    if (on) {
        i2c.rail_on_us = esp_timer_get_time();
        delay_at_least_ms(RAIL_SETTLE_MS);
    }
}

static void warn_once(const char *what, esp_err_t err)
{
    if (!i2c.warned) {
        i2c.warned = true;
        ESP_LOGW(TAG, "%s: %s (further I2C errors not logged)", what, esp_err_to_name(err));
    }
}

/* Run one idempotent transfer up to I2C_TRIES times. A failed try leaves the driver's state machine reset for
 * the next call (ESP-IDF 5.5.5 i2c_master.c, s_i2c_transaction_start). */
#define I2C_RETRY(call)                                                                    \
    ({                                                                                     \
        esp_err_t e_ = (call);                                                             \
        for (int t_ = 1; e_ != ESP_OK && t_ < I2C_TRIES; t_++) {                           \
            log_retry(e_);                                                                 \
            delay_at_least_ms(RETRY_PAUSE_MS);                                             \
            e_ = (call);                                                                   \
        }                                                                                  \
        e_;                                                                                \
    })

static void log_retry(esp_err_t err)
{
    if (!i2c.retry_logged) {
        i2c.retry_logged = true;
        ESP_LOGW(TAG, "transfer failed (%s), retrying: SCL %d SDA %d, %lld ms after the rail came on", esp_err_to_name(err),
                 gpio_get_level(WS154_I2C_SCL), gpio_get_level(WS154_I2C_SDA),
                 (long long)((esp_timer_get_time() - i2c.rail_on_us) / 1000));
    }
}

static i2c_master_dev_handle_t add_device(uint16_t addr)
{
    i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = I2C_HZ};
    i2c_master_dev_handle_t dev = NULL;
    return i2c_master_bus_add_device(i2c.bus, &cfg, &dev) == ESP_OK ? dev : NULL;
}

/* ---------------------------------------------------------------- PCF85063A */

static esp_err_t pcf_write(uint8_t reg, const uint8_t *data, size_t n)
{
    uint8_t buf[1 + BOARD_PCF85063_TIME_REGS];
    buf[0] = reg;
    for (size_t i = 0; i < n; i++) {
        buf[1 + i] = data[i];
    }
    return I2C_RETRY(i2c_master_transmit(i2c.rtc, buf, 1 + n, I2C_TIMEOUT_MS));
}

static bool rtc_setup(void)
{
    if (i2c_master_probe(i2c.bus, PCF85063_ADDR, I2C_TIMEOUT_MS) != ESP_OK || !(i2c.rtc = add_device(PCF85063_ADDR))) {
        return false;
    }
    const uint8_t reg = PCF_CONTROL_1, c2 = PCF_C2_CLKOUT_OFF;
    uint8_t c1;
    if (i2c_master_transmit_receive(i2c.rtc, &reg, 1, &c1, 1, I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    /* 24-hour mode and running. CAP_SEL is left as found (reset default 7 pF).
     * TODO(hw-verify): the load capacitance for crystal Y2 with C83/C84 (22 pF): measure the drift and
     * pick CAP_SEL / the Offset register. CLKOUT off: it is unused (and CLKOE is not driven). */
    i2c.control_1 = c1 & (uint8_t)~(PCF_C1_STOP | PCF_C1_12_24);
    i2c.rtc_valid_mode = !(c1 & PCF_C1_12_24);
    esp_err_t err = c1 != i2c.control_1 ? pcf_write(PCF_CONTROL_1, &i2c.control_1, 1) : ESP_OK;
    if (err == ESP_OK) {
        err = pcf_write(PCF_CONTROL_2, &c2, 1);
    }
    return err == ESP_OK;
}

static bool rtc_get(uint32_t *utc_s)
{
    const uint8_t reg = PCF_SECONDS;
    uint8_t r[BOARD_PCF85063_TIME_REGS];
    /* one burst: the chip freezes the time registers during the access (datasheet 7.4) */
    esp_err_t err = I2C_RETRY(i2c_master_transmit_receive(i2c.rtc, &reg, 1, r, sizeof(r), I2C_TIMEOUT_MS));
    if (err != ESP_OK) {
        warn_once("RTC read", err);
        return false;
    }
    return board_pcf85063_decode(r, utc_s); /* false when OS is set: the clock stopped since it was set */
}

static bool rtc_set(uint32_t utc_s)
{
    uint8_t r[BOARD_PCF85063_TIME_REGS];
    if (!board_pcf85063_encode(utc_s, r)) {
        return false;
    }
    /* datasheet 7.2.1.2: STOP resets the prescaler, write the time (OS cleared), then release STOP */
    const uint8_t stopped = i2c.control_1 | PCF_C1_STOP;
    esp_err_t err = pcf_write(PCF_CONTROL_1, &stopped, 1);
    if (err == ESP_OK) {
        err = pcf_write(PCF_SECONDS, r, sizeof(r));
    }
    esp_err_t run = pcf_write(PCF_CONTROL_1, &i2c.control_1, 1); /* always try to restart the clock */
    if (err != ESP_OK || run != ESP_OK) {
        warn_once("RTC write", err != ESP_OK ? err : run);
        return false;
    }
    i2c.rtc_valid_mode = true;
    return true;
}

/* ---------------------------------------------------------------- SHTC3 */

static esp_err_t sht_cmd(uint16_t cmd)
{
    const uint8_t b[2] = {(uint8_t)(cmd >> 8), (uint8_t)cmd};
    return i2c_master_transmit(i2c.sht, b, sizeof(b), I2C_TIMEOUT_MS);
}

static void sht_wake(void)
{
    (void)sht_cmd(SHTC3_WAKEUP); /* may not be acknowledged if it was already awake */
    esp_rom_delay_us(SHTC3_WAKE_US);
}

static bool sht_setup(void)
{
    if (!(i2c.sht = add_device(SHTC3_ADDR))) {
        return false;
    }
    /* no probe first: an SHTC3 left asleep (by us, before a reset) answers only the wakeup command */
    sht_wake();
    uint8_t id[3];
    const uint8_t cmd[2] = {SHTC3_READ_ID >> 8, SHTC3_READ_ID & 0xFF};
    bool ok = i2c_master_transmit_receive(i2c.sht, cmd, sizeof(cmd), id, sizeof(id), I2C_TIMEOUT_MS) == ESP_OK &&
              board_sensirion_crc8(id, 2) == id[2] && ((id[0] << 8 | id[1]) & SHTC3_ID_MASK) == SHTC3_ID;
    (void)sht_cmd(SHTC3_SLEEP); /* idle draws ~45 uA, sleep ~0.3 uA */
    return ok;
}

static bool read_climate(board_climate_t *out)
{
    uint8_t b[6];
    sht_wake();
    esp_err_t err = sht_cmd(SHTC3_MEASURE_T_FIRST);
    bool ok = false;
    if (err == ESP_OK) {
        delay_at_least_ms(SHTC3_MEASURE_MS);
        /* the sensor NACKs the read header until the measurement is done (datasheet 5.5) */
        for (int i = 0; i < SHTC3_POLLS && (err = i2c_master_receive(i2c.sht, b, sizeof(b), I2C_TIMEOUT_MS)) != ESP_OK;
             i++) {
            delay_at_least_ms(1);
        }
        ok = err == ESP_OK && board_shtc3_decode(b, out);
    }
    (void)sht_cmd(SHTC3_SLEEP);
    if (err != ESP_OK) {
        warn_once("SHTC3 read", err);
    }
    /* TODO(hw-verify): self-heating from the ESP32 and the battery next to the sensor; an offset may
     * be needed (compare with a reference thermometer, checklist item 18). */
    return ok;
}

/* ---------------------------------------------------------------- public: rail around each use */

bool ws154_rtc_get(void *ctx, uint32_t *utc_s)
{
    (void)ctx;
    if (!i2c.rtc || !i2c.rtc_valid_mode) {
        return false;
    }
    bus_power(true);
    bool ok = rtc_get(utc_s);
    bus_power(false);
    return ok;
}

bool ws154_rtc_set(void *ctx, uint32_t utc_s)
{
    (void)ctx;
    if (!i2c.rtc) {
        return false;
    }
    bus_power(true);
    bool ok = rtc_set(utc_s);
    bus_power(false);
    return ok;
}

bool ws154_read_climate(void *ctx, board_climate_t *out)
{
    (void)ctx;
    if (!i2c.sht) {
        return false;
    }
    bus_power(true);
    bool ok = read_climate(out);
    bus_power(false);
    return ok;
}

/* ---------------------------------------------------------------- ES8311 (register access for ws154_audio.c) */

void ws154_audio_rail(bool on) { bus_power(on); }

bool ws154_codec_write(uint8_t reg, uint8_t value)
{
    if (!i2c.bus) {
        return false;
    }
    if (!i2c.codec) {
        i2c.codec = add_device(WS154_CODEC_ADDR);
    }
    const uint8_t buf[2] = {reg, value};
    return i2c.codec && i2c_master_transmit(i2c.codec, buf, 2, I2C_TIMEOUT_MS) == ESP_OK;
}

/* ---------------------------------------------------------------- bus */

bool ws154_i2c_init(void)
{
    i2c_master_bus_config_t cfg = {.i2c_port = -1, .sda_io_num = WS154_I2C_SDA, .scl_io_num = WS154_I2C_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7};
    if (i2c_new_master_bus(&cfg, &i2c.bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus not created");
        return false;
    }
    bus_power(true);
    if (!rtc_setup() && i2c.rtc) {
        i2c_master_bus_rm_device(i2c.rtc);
        i2c.rtc = NULL;
    }
    if (!sht_setup() && i2c.sht) {
        i2c_master_bus_rm_device(i2c.sht);
        i2c.sht = NULL;
    }
    bus_power(false);
    ESP_LOGI(TAG, "RTC %s, SHTC3 %s", i2c.rtc ? "found" : "not found", i2c.sht ? "found" : "not found");
    return true;
}

bool ws154_rtc_present(void) { return i2c.rtc != NULL; }
bool ws154_climate_present(void) { return i2c.sht != NULL; }
