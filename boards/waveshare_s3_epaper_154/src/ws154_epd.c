/*
 * E-paper driver, 200x200 B/W, SSD168x command set (SSD1681 class).
 *
 * Written from the controller's command set, NOT copied from the Waveshare demo (the demo repo has
 * no licence). Differences from the demo: it uses the controller's built-in (OTP) waveforms
 * (Display Update Control 0xF7 for a full and 0xFC for a partial update) where the demo loads its
 * own LUT tables with 0xC7 / 0xCF.
 *
 * TODO(verify): the controller model (the SSD1681 datasheet is not read yet; the schematic only
 * shows the 24-pin FPC) and every command value below, against its datasheet.
 * TODO(hw-verify): that the OTP waveforms give a clean partial update on this panel. If not, fall
 * back to the demo's custom-LUT path (docs/hw-checklist.md item 7). Nothing in this file has run on
 * hardware yet.
 *
 * Frame format (board_api.h): 1 = white, MSB = leftmost pixel, rows top to bottom: the same bit
 * layout the controller's RAM expects (1 = white) in the entry mode chosen below.
 *
 * Power: the panel rail is switched off when the panel sleeps, which clears the controller RAM, so
 * the driver keeps the frame that is on the panel (prev) and rewrites it into the "old" RAM
 * (0x26) with every new frame; the partial waveform compares old and new.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "epager/proto_gen.h"
#include "ws154_epd.h"
#include "ws154_pins.h"

#define TAG "ws154_epd"

#define EPD_W 200
#define EPD_H 200
#define FRAME_BYTES ((EPD_W / 8) * EPD_H) /* 5000 */
#define SPI_HZ (10 * 1000 * 1000)         /* TODO(verify): the controller's maximum SCL rate */
#define SPI_CHUNK 500                     /* DMA-capable bounce buffer: frames may sit in PSRAM */
#define BUSY_TIMEOUT_MS 5000
#define BUSY_MIN_MS 5 /* BUSY needs a moment to rise after "master activation" */

/* SSD168x commands used */
#define CMD_DRIVER_OUTPUT 0x01
#define CMD_DEEP_SLEEP 0x10
#define CMD_ENTRY_MODE 0x11
#define CMD_SWRESET 0x12
#define CMD_ACTIVATE 0x20
#define CMD_WRITE_NEW 0x24 /* "BW" RAM: the new image */
#define CMD_WRITE_OLD 0x26 /* "RED" RAM: the previous image, used by the partial waveform */
#define CMD_UPDATE_CTRL2 0x22
#define CMD_BORDER 0x3C
#define CMD_TEMP_SENSOR 0x18
#define CMD_RAM_X_RANGE 0x44
#define CMD_RAM_Y_RANGE 0x45
#define CMD_RAM_X_COUNT 0x4E
#define CMD_RAM_Y_COUNT 0x4F

#define UPDATE_FULL 0xF7
#define UPDATE_PARTIAL 0xFC

static struct {
    spi_device_handle_t spi;
    bool ready;      /* SPI up and panel initialised */
    bool primed;     /* the analog supply was powered once since the last reset (needed before a partial) */
    int64_t busy_from_us; /* when the last refresh was started */
    bool refreshing;
    uint8_t prev[FRAME_BYTES]; /* what the panel shows (0xFF = white) */
} epd;

DMA_ATTR static uint8_t txbuf[SPI_CHUNK];

/* ---------------------------------------------------------------- low level */

static void tx(const uint8_t *data, size_t len, bool dc)
{
    gpio_set_level(WS154_EPD_DC, dc ? 1 : 0);
    while (len > 0) {
        size_t n = len < SPI_CHUNK ? len : SPI_CHUNK;
        memcpy(txbuf, data, n);
        spi_transaction_t t = {.length = n * 8, .tx_buffer = txbuf};
        esp_err_t err = spi_device_polling_transmit(epd.spi, &t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi: %s", esp_err_to_name(err));
            return;
        }
        data += n;
        len -= n;
    }
}

static void cmd(uint8_t c) { tx(&c, 1, false); }
static void dat(uint8_t d) { tx(&d, 1, true); }

static bool pin_busy(void) { return gpio_get_level(WS154_EPD_BUSY) != 0; }

static bool wait_idle(void)
{
    int64_t end = esp_timer_get_time() + BUSY_TIMEOUT_MS * 1000LL;
    while (pin_busy()) {
        if (esp_timer_get_time() > end) {
            ESP_LOGE(TAG, "panel stuck busy");
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

/* Cursor at the top-left of a window covering the whole panel, entry mode "x+, y+". */
static void set_window(void)
{
    cmd(CMD_ENTRY_MODE);
    dat(0x03);
    cmd(CMD_RAM_X_RANGE);
    dat(0);
    dat(EPD_W / 8 - 1);
    cmd(CMD_RAM_Y_RANGE);
    dat(0);
    dat(0);
    dat((EPD_H - 1) & 0xFF);
    dat((EPD_H - 1) >> 8);
    cmd(CMD_RAM_X_COUNT);
    dat(0);
    cmd(CMD_RAM_Y_COUNT);
    dat(0);
    dat(0);
}

static bool panel_reset_and_configure(void)
{
    gpio_set_level(WS154_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(WS154_EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(WS154_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!wait_idle()) {
        return false;
    }
    cmd(CMD_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!wait_idle()) {
        return false;
    }
    cmd(CMD_DRIVER_OUTPUT);
    dat((EPD_H - 1) & 0xFF);
    dat((EPD_H - 1) >> 8);
    dat(0x00);
    cmd(CMD_BORDER);
    dat(0x05);
    cmd(CMD_TEMP_SENSOR);
    dat(0x80); /* internal temperature sensor */
    set_window();
    epd.primed = false;
    return true;
}

/* ---------------------------------------------------------------- board_display_t */

static void get_info(void *ctx, board_display_info_t *out)
{
    (void)ctx;
    out->width = EPD_W;
    out->height = EPD_H;
    out->display_type = PROTO_DISPLAY_TYPE_EPAPER_BW;
    out->color_depth = 1;
    out->refresh_class = PROTO_REFRESH_CLASS_EPAPER;
}

static bool epd_rail(bool on)
{
    gpio_set_level(WS154_EPD_PWR, on ? 0 : 1); /* active low (PMOS) */
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(10)); /* TODO(hw-verify): rail settle time */
    }
    return true;
}

static bool init(void *ctx)
{
    (void)ctx;
    if (epd.ready) {
        return true;
    }
    gpio_config_t out = {.pin_bit_mask = (1ULL << WS154_EPD_RST) | (1ULL << WS154_EPD_DC) | (1ULL << WS154_EPD_PWR),
                         .mode = GPIO_MODE_OUTPUT};
    gpio_config_t in = {.pin_bit_mask = 1ULL << WS154_EPD_BUSY, .mode = GPIO_MODE_INPUT};
    gpio_set_level(WS154_EPD_PWR, 1); /* rail stays off until the SPI pins are configured */
    if (gpio_config(&out) != ESP_OK || gpio_config(&in) != ESP_OK) {
        return false;
    }
    spi_bus_config_t bus = {.mosi_io_num = WS154_EPD_SDI, .miso_io_num = -1, .sclk_io_num = WS154_EPD_SCLK,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = SPI_CHUNK};
    spi_device_interface_config_t dev = {.clock_speed_hz = SPI_HZ, .mode = 0, .spics_io_num = WS154_EPD_CS,
                                         .queue_size = 1};
    if (spi_bus_initialize(WS154_EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK ||
        spi_bus_add_device(WS154_EPD_SPI_HOST, &dev, &epd.spi) != ESP_OK) {
        return false;
    }
    memset(epd.prev, 0xFF, sizeof(epd.prev));
    epd_rail(true);
    epd.ready = panel_reset_and_configure();
    return epd.ready;
}

static void write_frame(void *ctx, const uint8_t *frame)
{
    (void)ctx;
    if (!epd.ready) {
        return;
    }
    set_window();
    cmd(CMD_WRITE_OLD);
    tx(epd.prev, FRAME_BYTES, true);
    set_window();
    cmd(CMD_WRITE_NEW);
    tx(frame, FRAME_BYTES, true);
    memcpy(epd.prev, frame, FRAME_BYTES);
}

static void refresh(void *ctx, board_refresh_t kind)
{
    (void)ctx;
    if (!epd.ready) {
        return;
    }
    bool full = kind == BOARD_REFRESH_FULL;
    if (!full && !epd.primed) {
        /* TODO(verify): power-up step before the first partial after a reset (clock + analog on). */
        cmd(CMD_UPDATE_CTRL2);
        dat(0xC0);
        cmd(CMD_ACTIVATE);
        wait_idle();
    }
    epd.primed = true;
    cmd(CMD_BORDER);
    dat(full ? 0x05 : 0x80);
    cmd(CMD_UPDATE_CTRL2);
    dat(full ? UPDATE_FULL : UPDATE_PARTIAL);
    cmd(CMD_ACTIVATE);
    epd.refreshing = true;
    epd.busy_from_us = esp_timer_get_time();
}

static bool busy(void *ctx)
{
    (void)ctx;
    if (!epd.refreshing) {
        return false;
    }
    if (esp_timer_get_time() - epd.busy_from_us < BUSY_MIN_MS * 1000LL || pin_busy()) {
        return true;
    }
    epd.refreshing = false;
    return false;
}

static board_refresh_t choose_refresh(void *ctx, uint32_t partials_since_full, uint32_t full_every)
{
    (void)ctx;
    return partials_since_full >= (full_every ? full_every : 1u) ? BOARD_REFRESH_FULL : BOARD_REFRESH_PARTIAL;
}

static void panel_sleep(void *ctx)
{
    (void)ctx;
    if (!epd.ready) {
        return;
    }
    cmd(CMD_DEEP_SLEEP);
    dat(0x01);
    epd_rail(false);
    epd.ready = false;
}

static void panel_wake(void *ctx)
{
    (void)ctx;
    if (epd.ready || epd.spi == NULL) {
        return;
    }
    epd_rail(true);
    epd.ready = panel_reset_and_configure();
}

static const board_display_t display = {
    .get_info = get_info, .init = init, .write_frame = write_frame, .refresh = refresh, .busy = busy,
    .choose_refresh = choose_refresh, .sleep = panel_sleep, .wake = panel_wake, .ctx = NULL};

const board_display_t *ws154_epd_get(void) { return &display; }
