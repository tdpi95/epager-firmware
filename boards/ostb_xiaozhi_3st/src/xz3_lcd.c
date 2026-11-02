/*
 * LCD driver of the ostb-xiaozhi-3st: 240 x 296 panel used as 296 x 240 landscape, RGB565 over
 * write-only SPI (ESP-IDF's esp_lcd SPI panel IO with DMA). The controller is in doubt (NV3023 or a
 * JD9853-class part, docs/ostb-xiaozhi-3st.md), so the panel is driven exactly as the stock firmware
 * drives it: same reset timing, same command order, same init list. TODO(hw-verify): everything here, hw-checklist items 21-24.
 *
 * Area path of board_api (D46): write_area copies the caller's pixels into two internal DMA band
 * buffers, swapping the bytes to the panel's big-endian order, and sends band after band; copying the
 * next band overlaps the transfer of the previous one.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "epager/proto_gen.h"
#include "xz3_lcd.h"
#include "xz3_pins.h"

#define TAG "xz3_lcd"

#define BAND_LINES 24                            /* full-width lines per DMA band */
#define BAND_PIXELS (XZ3_LCD_W * BAND_LINES)     /* 7104 pixels, 14208 bytes per buffer */
#define MADCTL_LANDSCAPE (LCD_CMD_MV_BIT | LCD_CMD_MY_BIT) /* stock: swap_xy, mirror y, RGB order */
/* Turned by 180 degrees (D58): both mirror bits toggled; the 24-pixel gap moves to the other end of the
 * 320-pixel axis. TODO(hw-verify): checklist item 29. */
#define MADCTL_FLIPPED (LCD_CMD_MV_BIT | LCD_CMD_MX_BIT)
#define X_GAP_FLIPPED (320 - XZ3_LCD_W - XZ3_LCD_X_GAP)
#define BL_TIMER LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_RES LEDC_TIMER_10_BIT                 /* 25 kHz from the 80 MHz APB clock allows 11 bits */
#define BL_MAX ((1u << 10) - 1u)

/* The stock firmware's init list (docs/ostb-xiaozhi-3st.md, "Stock init sequence"): the vendor's
 * values for this panel. DF 98 53 unlocks the vendor registers, DE selects the register page. */
typedef struct {
    uint8_t cmd, len;
    uint16_t delay_ms;
    uint8_t data[32];
} init_cmd_t;

static const init_cmd_t STOCK_INIT[] = {
    {0xDF, 2, 0, {0x98, 0x53}},
    {0xDE, 1, 0, {0x00}},
    {0xB2, 1, 0, {0x25}},
    {0xB7, 4, 0, {0x00, 0x29, 0x00, 0x51}},
    {0xBB, 6, 0, {0x4F, 0x1A, 0x55, 0x73, 0x63, 0xF0}},
    {0xC0, 2, 0, {0x44, 0xA4}},
    {0xC1, 1, 0, {0x12}},
    {0xC3, 8, 0, {0x7D, 0x07, 0x14, 0x06, 0xC8, 0x71, 0x6C, 0x77}},
    {0xC4, 12, 0, {0x00, 0x00, 0x94, 0x79, 0x25, 0x0A, 0x16, 0x79, 0x25, 0x0A, 0x16, 0x82}},
    {0xC8, 32, 0, {0x3F, 0x34, 0x2D, 0x26, 0x2B, 0x2B, 0x25, 0x24, 0x23, 0x22, 0x20, 0x17, 0x14, 0x0E, 0x06, 0x00,
                   0x3F, 0x34, 0x2D, 0x26, 0x2B, 0x2B, 0x25, 0x24, 0x23, 0x22, 0x20, 0x17, 0x14, 0x0E, 0x06, 0x00}},
    {0xD0, 5, 0, {0x04, 0x06, 0x6B, 0x0F, 0x00}},
    {0xD7, 2, 0, {0x00, 0x30}},
    {0xE6, 1, 0, {0x10}},
    {0xDE, 1, 0, {0x01}},
    {0xB7, 5, 0, {0x03, 0x13, 0xEF, 0x35, 0x35}},
    {0xC1, 3, 0, {0x14, 0x15, 0xC0}},
    {0xC2, 2, 0, {0x06, 0x3A}},
    {0xC4, 2, 0, {0x72, 0x12}},
    {0xBE, 1, 0, {0x00}},
    {0xDE, 1, 0, {0x00}},
    {0x35, 1, 0, {0x00}},                    /* tearing-effect output on, V-blank only */
    {0x3A, 1, 0, {0x05}},                    /* RGB565 */
    {0x2A, 4, 0, {0x00, 0x00, 0x00, 0xEF}},  /* columns 0-239 (native orientation) */
    {0x2B, 4, 0, {0x00, 0x00, 0x01, 0x27}},  /* rows 0-295 */
    {0x11, 0, 120, {0}},                     /* sleep out */
};

static struct {
    esp_lcd_panel_io_handle_t io;
    uint16_t *band[2];
    uint8_t next;              /* band buffer to fill next; the other one may still be in flight */
    bool ready, asleep, flipped;
} lcd;

static bool cmd(uint8_t c, const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(lcd.io, c, data, len) == ESP_OK;
}

/* ---------------------------------------------------------------- backlight */

static bool backlight_init(void)
{
    ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = BL_RES, .timer_num = BL_TIMER,
                             .freq_hz = XZ3_LCD_BL_HZ, .clk_cfg = LEDC_AUTO_CLK};
    ledc_channel_config_t c = {.gpio_num = XZ3_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = BL_CHANNEL,
                               .timer_sel = BL_TIMER, .duty = 0, .hpoint = 0};
    return ledc_timer_config(&t) == ESP_OK && ledc_channel_config(&c) == ESP_OK;
}

static void set_backlight(void *ctx, uint8_t pct)
{
    (void)ctx;
    if (pct > 100) {
        pct = 100;
    }
    if (pct == 0) {
        /* Off is a steady low, not a 0 % duty cycle: PWM does not run in light sleep (PLAN D17), and a pin
         * that is merely "duty 0" is left to whatever the stopped timer shows. */
        ledc_stop(LEDC_LOW_SPEED_MODE, BL_CHANNEL, 0);
        return;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, BL_MAX * pct / 100u);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL); /* also switches the output on again after ledc_stop */
}

/* ---------------------------------------------------------------- pixels */

static bool set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    const uint16_t x0 = x + (lcd.flipped ? X_GAP_FLIPPED : XZ3_LCD_X_GAP), x1 = x0 + w - 1;
    const uint16_t y0 = y + XZ3_LCD_Y_GAP, y1 = y0 + h - 1;
    const uint8_t ca[] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    const uint8_t ra[] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    return cmd(LCD_CMD_CASET, ca, sizeof(ca)) && cmd(LCD_CMD_RASET, ra, sizeof(ra));
}

/*
 * One band at a time: fill the free buffer, then set the window and queue the transfer. Setting the
 * window waits for the transfer in flight (esp_lcd SPI IO waits for queued colour transfers before a
 * parameter write), so at most one band is in flight and it is never in the buffer being filled.
 */
static void write_area(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *px)
{
    (void)ctx;
    if (!lcd.ready || x >= XZ3_LCD_W || y >= XZ3_LCD_H || w == 0 || h == 0) {
        return;
    }
    const uint16_t stride = w;
    if (w > XZ3_LCD_W - x) {
        w = XZ3_LCD_W - x;
    }
    if (h > XZ3_LCD_H - y) {
        h = XZ3_LCD_H - y;
    }
    const uint16_t rows_per_band = (uint16_t)(BAND_PIXELS / w);
    for (uint16_t row = 0; row < h;) {
        const uint16_t rows = h - row < rows_per_band ? h - row : rows_per_band;
        uint16_t *buf = lcd.band[lcd.next];
        for (uint16_t r = 0; r < rows; r++) {
            const uint16_t *src = px + (size_t)(row + r) * stride;
            uint16_t *dst = buf + (size_t)r * w;
            for (uint16_t i = 0; i < w; i++) {
                dst[i] = (uint16_t)(src[i] >> 8 | src[i] << 8);
            }
        }
        if (!set_window(x, y + row, w, rows) ||
            esp_lcd_panel_io_tx_color(lcd.io, LCD_CMD_RAMWR, buf, (size_t)rows * w * 2u) != ESP_OK) {
            ESP_LOGE(TAG, "write failed");
            return;
        }
        lcd.next ^= 1u;
        row += rows;
    }
}

static void fill(uint16_t rgb565)
{
    static uint16_t line[XZ3_LCD_W * 8];
    for (size_t i = 0; i < sizeof(line) / sizeof(line[0]); i++) {
        line[i] = rgb565;
    }
    for (uint16_t y = 0; y < XZ3_LCD_H; y += 8) {
        write_area(NULL, 0, y, XZ3_LCD_W, 8, line);
    }
}

/* ---------------------------------------------------------------- board_display_t */

static void get_info(void *ctx, board_display_info_t *out)
{
    (void)ctx;
    *out = (board_display_info_t){.width = XZ3_LCD_W, .height = XZ3_LCD_H,
                                  .display_type = PROTO_DISPLAY_TYPE_LCD_COLOR, .color_depth = 16,
                                  .refresh_class = PROTO_REFRESH_CLASS_LCD,
                                  .edge_inset = XZ3_LCD_EDGE_INSET};
}

static bool init_bus(void)
{
    spi_bus_config_t bus = {.mosi_io_num = XZ3_LCD_MOSI, .miso_io_num = -1, .sclk_io_num = XZ3_LCD_SCLK,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = BAND_PIXELS * 2};
    esp_lcd_panel_io_spi_config_t io = {.cs_gpio_num = XZ3_LCD_CS, .dc_gpio_num = XZ3_LCD_DC, .spi_mode = 0,
                                        .pclk_hz = XZ3_LCD_SPI_HZ, .trans_queue_depth = 2,
                                        .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    gpio_config_t rst = {.pin_bit_mask = 1ULL << XZ3_LCD_RST, .mode = GPIO_MODE_OUTPUT};
    return spi_bus_initialize(XZ3_LCD_HOST, &bus, SPI_DMA_CH_AUTO) == ESP_OK &&
           esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)XZ3_LCD_HOST, &io, &lcd.io) == ESP_OK &&
           gpio_config(&rst) == ESP_OK;
}

static bool init(void *ctx)
{
    (void)ctx;
    if (lcd.ready) {
        return true;
    }
    for (int i = 0; i < 2; i++) {
        lcd.band[i] = heap_caps_malloc(BAND_PIXELS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    if (lcd.band[0] == NULL || lcd.band[1] == NULL || !backlight_init() || !init_bus()) {
        ESP_LOGE(TAG, "LCD bus setup failed");
        return false;
    }
    /* Hardware reset as the stock firmware does it: 10 ms low, then 120 ms before the first command.
     * TODO(hw-verify): the community port saw garbage after some resets (checklist item 22). */
    gpio_set_level(XZ3_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(XZ3_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* The stock order: sleep out, MADCTL and COLMOD defaults, the init list (which ends with sleep
     * out), then orientation and inversion off. Display on only after the RAM holds a black frame,
     * so the panel never shows what the RAM held at power-up. */
    bool ok = cmd(LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ok = ok && cmd(LCD_CMD_MADCTL, (const uint8_t[]){0x00}, 1) && cmd(LCD_CMD_COLMOD, (const uint8_t[]){0x55}, 1);
    for (size_t i = 0; ok && i < sizeof(STOCK_INIT) / sizeof(STOCK_INIT[0]); i++) {
        ok = cmd(STOCK_INIT[i].cmd, STOCK_INIT[i].len ? STOCK_INIT[i].data : NULL, STOCK_INIT[i].len);
        if (STOCK_INIT[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(STOCK_INIT[i].delay_ms));
        }
    }
    ok = ok && cmd(LCD_CMD_MADCTL, (const uint8_t[]){MADCTL_LANDSCAPE}, 1) && cmd(LCD_CMD_INVOFF, NULL, 0);
    if (!ok) {
        ESP_LOGE(TAG, "LCD init commands failed");
        return false;
    }
    lcd.ready = true;
    fill(0x0000);
    ok = cmd(LCD_CMD_DISPON, NULL, 0);
    ESP_LOGI(TAG, "LCD %dx%d ready, SPI %d MHz", XZ3_LCD_W, XZ3_LCD_H, XZ3_LCD_SPI_HZ / 1000000);
    return ok;
}

static board_refresh_t choose_refresh(void *ctx, uint32_t partials_since_full, uint32_t full_every)
{
    (void)ctx;
    (void)partials_since_full;
    (void)full_every;
    return BOARD_REFRESH_FULL; /* an LCD has no partial refresh */
}

static void panel_sleep(void *ctx)
{
    if (!lcd.ready || lcd.asleep) {
        return;
    }
    set_backlight(ctx, 0);
    cmd(LCD_CMD_DISPOFF, NULL, 0);
    cmd(LCD_CMD_SLPIN, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120)); /* MIPI DCS: wait before the next sleep out */
    lcd.asleep = true;
}

/* The caller sets the backlight again once a frame is on the panel. */
static void panel_wake(void *ctx)
{
    (void)ctx;
    if (!lcd.ready || !lcd.asleep) {
        return;
    }
    cmd(LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(LCD_CMD_DISPON, NULL, 0);
    lcd.asleep = false;
}

/* 0 and 180 degrees (the nearest of 1 and 3); the touch driver mirrors its points to match. */
static void set_rotation(void *ctx, uint8_t quarter_turns)
{
    (void)ctx;
    const bool flipped = quarter_turns >= 2;
    if (!lcd.ready || flipped == lcd.flipped) {
        return;
    }
    lcd.flipped = flipped;
    cmd(LCD_CMD_MADCTL, (const uint8_t[]){flipped ? MADCTL_FLIPPED : MADCTL_LANDSCAPE}, 1);
}

bool xz3_lcd_flipped(void) { return lcd.flipped; }

static const board_display_t display = {
    .get_info = get_info, .init = init, .choose_refresh = choose_refresh, .sleep = panel_sleep,
    .wake = panel_wake, .write_area = write_area, .set_backlight = set_backlight, .set_rotation = set_rotation,
    .ctx = NULL};

const board_display_t *xz3_lcd_get(void) { return &display; }
