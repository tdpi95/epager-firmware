/*
 * I2C bus and CST816S touch controller of the ostb-xiaozhi-3st. The bus also carries the ES8311
 * codec and the ES7210 microphone ADC, which ePager leaves alone. The stock firmware runs the bus
 * with internal pull-ups, reads the touch chip at 400 kHz and uses neither a reset nor an interrupt
 * pin, so the touch point is polled. The data layout is decoded by board_cst816s_decode (host-tested).
 * Orientation and calibration checked on the board (hw-checklist item 25). TODO(hw-verify): whether the
 * chip still answers after a long time untouched (its own low-power mode).
 */
#include "xz3_touch.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "epager/board_util.h"
#include "xz3_lcd.h"
#include "xz3_pins.h"

#define TAG "xz3_touch"
#define I2C_TIMEOUT_MS 20

/* Calibration from 28 taps on 9 crosshairs (2026-10-13, hw-checklist item 25): the chip reports
 * x = 1.0184 * true_x - 0.49 and y = 1.0449 * true_y - 1.88 (least squares; mostly the bottom edge,
 * +10 px). Inverted here in thousandths. The remaining error (about 5 px) is the scatter of the taps. */
#define CAL_X_GAIN 1018
#define CAL_X_OFS 0
#define CAL_Y_GAIN 1045
#define CAL_Y_OFS 2

static uint16_t calibrate(uint16_t v, int gain, int ofs, uint16_t max)
{
    int c = ((int)v + ofs) * 1000 / gain;
    return (uint16_t)(c < 0 ? 0 : c > max ? max : c);
}

static struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t touch;
    bool warned;
} t;

bool xz3_i2c_init(void)
{
    i2c_master_bus_config_t cfg = {.i2c_port = XZ3_I2C_PORT, .sda_io_num = XZ3_I2C_SDA, .scl_io_num = XZ3_I2C_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                   .flags.enable_internal_pullup = true};
    if (i2c_new_master_bus(&cfg, &t.bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return false;
    }
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = XZ3_TOUCH_ADDR,
                               .scl_speed_hz = XZ3_TOUCH_HZ};
    if (i2c_master_bus_add_device(t.bus, &dev, &t.touch) != ESP_OK) {
        t.touch = NULL;
        return false;
    }
    /* Log which chips answer: a cheap check of the pins and addresses on the first boot. */
    const uint16_t addrs[] = {XZ3_TOUCH_ADDR, XZ3_ES8311_ADDR, XZ3_ES7210_ADDR};
    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        esp_err_t err = i2c_master_probe(t.bus, addrs[i], I2C_TIMEOUT_MS);
        ESP_LOGI(TAG, "I2C 0x%02x: %s", addrs[i], err == ESP_OK ? "answers" : esp_err_to_name(err));
    }
    return true;
}

bool xz3_touch_read(void *ctx, board_touch_t *out)
{
    (void)ctx;
    uint8_t reg = BOARD_CST816S_DATA_REG, b[BOARD_CST816S_DATA_LEN];
    if (t.touch == NULL) {
        return false;
    }
    esp_err_t err = i2c_master_transmit_receive(t.touch, &reg, 1, b, sizeof(b), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        if (!t.warned) {
            t.warned = true;
            ESP_LOGW(TAG, "touch read: %s (further errors not logged)", esp_err_to_name(err));
        }
        return false;
    }
    /* The controller's axes match the 296 x 240 landscape screen (no swap or mirror, as the stock
     * firmware has it; confirmed on the board), with the small calibration above. */
    board_cst816s_decode(b, out);
    if (out->pressed) {
        out->x = calibrate(out->x, CAL_X_GAIN, CAL_X_OFS, XZ3_LCD_W - 1);
        out->y = calibrate(out->y, CAL_Y_GAIN, CAL_Y_OFS, XZ3_LCD_H - 1);
        if (xz3_lcd_flipped()) { /* the panel is turned by 180 degrees: so are the points */
            out->x = (uint16_t)(XZ3_LCD_W - 1 - out->x);
            out->y = (uint16_t)(XZ3_LCD_H - 1 - out->y);
        }
    }
    return true;
}
