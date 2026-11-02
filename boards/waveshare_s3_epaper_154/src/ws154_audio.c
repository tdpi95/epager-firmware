/*
 * Speaker: ES8311 codec (slave) fed by the ESP32's I2S master, then the NS4150B amplifier (PLAN D89).
 * Register values: ES8311 datasheet and the Apache-2.0 espressif/es8311 driver 1.0.0~1 (read, not linked: it
 * uses the legacy I2C driver, which cannot be mixed with the one this firmware uses), for the one mode needed:
 * playback only, 16 kHz, 16 bit, MCLK from the pin at 256 x fs. Nothing here has run on the board:
 * TODO(hw-verify) hw-checklist 71 (is there sound, how loud, any pop at the start or the end).
 */
#include "ws154_audio.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ws154_i2c.h"
#include "ws154_pins.h"

#define TAG "ws154_audio"

#define RATE_HZ 16000u
#define CHUNK_FRAMES 256
#define VOLUME_REG CONFIG_EPAGER_BOOT_SOUND_DAC_VOLUME /* ES8311 register 0x32, 0 = -95.5 dB in 0.5 dB steps */

typedef struct {
    uint8_t reg, val;
} reg_t;

/* After the reset and the clock settings: format, power up, DAC on, no equalizer, unmuted. */
static const reg_t codec_regs[] = {
    {0x01, 0x3F}, /* all clocks on, MCLK from the pin */
    {0x02, 0x00}, /* pre-divider 1, multiplier 1 (4.096 MHz MCLK / 16 kHz) */
    {0x03, 0x10}, /* ADC: single speed, OSR 16 */
    {0x04, 0x10}, /* DAC OSR 16 */
    {0x05, 0x00}, /* ADC and DAC clock divider 1 */
    {0x06, 0x03}, /* BCLK divider 4, not inverted */
    {0x07, 0x00}, /* LRCK divider high bits */
    {0x08, 0xFF}, /* LRCK divider low bits: 256 */
    {0x00, 0x80}, /* power on, serial port in slave mode */
    {0x09, 0x0C}, /* DAC input: I2S, 16 bit */
    {0x0A, 0x0C}, /* ADC output: I2S, 16 bit (unused) */
    {0x0D, 0x01}, /* analog circuits on */
    {0x0E, 0x02}, /* PGA and ADC modulator on */
    {0x12, 0x00}, /* DAC on */
    {0x13, 0x10}, /* output driver on */
    {0x1C, 0x6A}, /* ADC equalizer bypass, DC offset removal */
    {0x37, 0x08}, /* DAC equalizer bypass */
    {0x31, 0x00}, /* DAC not muted */
    {0x32, VOLUME_REG},
};

static bool codec_start(void)
{
    if (!ws154_codec_write(0x00, 0x1F)) { /* reset everything */
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!ws154_codec_write(0x00, 0x00)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(codec_regs) / sizeof(codec_regs[0]); i++) {
        if (!ws154_codec_write(codec_regs[i].reg, codec_regs[i].val)) {
            return false;
        }
    }
    return true;
}

bool ws154_audio_init_pins(void)
{
    gpio_set_level(WS154_PA_CTRL, 0);
    const gpio_config_t out = {.pin_bit_mask = 1ULL << WS154_PA_CTRL, .mode = GPIO_MODE_OUTPUT};
    return gpio_config(&out) == ESP_OK && gpio_sleep_sel_dis(WS154_PA_CTRL) == ESP_OK;
}

bool ws154_audio_play(void *ctx, board_audio_fn sample, void *user, uint32_t n_samples, uint32_t rate_hz)
{
    (void)ctx;
    if (rate_hz != RATE_HZ || sample == NULL) {
        return false;
    }
    bool ok = false;
    i2s_chan_handle_t tx = NULL;
    ws154_audio_rail(true);

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan.auto_clear = true; /* silence, not a repeat of the last buffer, if the writer is late */
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE_HZ), /* MCLK = 256 x fs */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = WS154_I2S_MCLK, .bclk = WS154_I2S_BCLK, .ws = WS154_I2S_LRCK, .dout = WS154_I2S_DOUT,
                     .din = I2S_GPIO_UNUSED},
    };
    if (i2s_new_channel(&chan, &tx, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "no I2S channel");
        tx = NULL;
    } else if (i2s_channel_init_std_mode(tx, &std) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) { /* MCLK runs */
        ESP_LOGE(TAG, "I2S did not start");
    } else if (!codec_start()) {
        ESP_LOGE(TAG, "ES8311 did not answer");
    } else {
        ok = true;
        int16_t frames[CHUNK_FRAMES * 2];
        gpio_set_level(WS154_PA_CTRL, 1); /* the amplifier, after the codec is up: no start-up pop */
        for (uint32_t i = 0; i < n_samples;) {
            uint32_t n = n_samples - i < CHUNK_FRAMES ? n_samples - i : CHUNK_FRAMES;
            for (uint32_t k = 0; k < n; k++) {
                frames[2 * k] = frames[2 * k + 1] = sample(user, i + k);
            }
            size_t written = 0;
            if (i2s_channel_write(tx, frames, n * 4, &written, 1000) != ESP_OK) {
                ok = false;
                break;
            }
            i += n;
        }
        vTaskDelay(pdMS_TO_TICKS(60)); /* the DMA buffers still hold the end of the sound */
        gpio_set_level(WS154_PA_CTRL, 0);
        ws154_codec_write(0x31, 0x60); /* mute */
    }
    if (tx != NULL) {
        i2s_channel_disable(tx);
        i2s_del_channel(tx);
    }
    gpio_reset_pin(WS154_I2S_MCLK); /* the pins go back to inputs: the codec is about to lose its power */
    gpio_reset_pin(WS154_I2S_BCLK);
    gpio_reset_pin(WS154_I2S_LRCK);
    gpio_reset_pin(WS154_I2S_DOUT);
    ws154_audio_rail(false);
    return ok;
}
