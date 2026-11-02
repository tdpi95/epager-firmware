#include "epager/sim_lcd.h"

#include <string.h>

#include "epager/proto_gen.h"

#define N_PIXELS (SIM_LCD_W * SIM_LCD_H)

static struct {
    bool inited, asleep;
    uint8_t backlight, rotation;
    uint16_t ram[N_PIXELS];
    uint32_t writes, violations;
    uint64_t pixels;
    uint16_t last_x, last_y, last_w, last_h;
    board_touch_t touch;
    bool battery_readable;
    uint8_t battery_pct, external_power;
    board_button_cb button_cb;
    void *button_user;
} sim;

/* ---------------------------------------------------------------- display driver (area path) */

static void get_info(void *ctx, board_display_info_t *out)
{
    (void)ctx;
    *out = (board_display_info_t){.width = SIM_LCD_W, .height = SIM_LCD_H,
                                  .display_type = PROTO_DISPLAY_TYPE_LCD_COLOR, .color_depth = 16,
                                  .refresh_class = PROTO_REFRESH_CLASS_LCD,
                                  .edge_inset = SIM_LCD_EDGE_INSET};
}

static bool init(void *ctx)
{
    (void)ctx;
    sim.inited = true;
    sim.asleep = false;
    return true;
}

static board_refresh_t choose_refresh(void *ctx, uint32_t partials_since_full, uint32_t full_every)
{
    (void)ctx;
    (void)partials_since_full;
    (void)full_every;
    return BOARD_REFRESH_FULL;
}

static void panel_sleep(void *ctx)
{
    (void)ctx;
    sim.asleep = true;
}

static void panel_wake(void *ctx)
{
    (void)ctx;
    sim.asleep = false;
}

/* Same contract as the real driver: copies, clips to the panel, the caller's stride is w. */
static void write_area(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *px)
{
    (void)ctx;
    if (!sim.inited || sim.asleep) {
        sim.violations++;
        return;
    }
    if (x >= SIM_LCD_W || y >= SIM_LCD_H || w == 0 || h == 0) {
        return;
    }
    const uint16_t stride = w;
    w = w > SIM_LCD_W - x ? (uint16_t)(SIM_LCD_W - x) : w;
    h = h > SIM_LCD_H - y ? (uint16_t)(SIM_LCD_H - y) : h;
    for (uint16_t r = 0; r < h; r++) {
        memcpy(&sim.ram[(uint32_t)(y + r) * SIM_LCD_W + x], px + (uint32_t)r * stride, (size_t)w * 2u);
    }
    sim.writes++;
    sim.pixels += (uint64_t)w * h;
    sim.last_x = x;
    sim.last_y = y;
    sim.last_w = w;
    sim.last_h = h;
}

static void set_backlight(void *ctx, uint8_t pct)
{
    (void)ctx;
    sim.backlight = pct > 100 ? 100 : pct;
}

static void set_rotation(void *ctx, uint8_t quarter_turns)
{
    (void)ctx;
    sim.rotation = quarter_turns >= 2 ? 2 : 0; /* 0 and 180 degrees, nearest of 1 and 3 */
}

static const board_display_t display = {
    .get_info = get_info, .init = init, .choose_refresh = choose_refresh, .sleep = panel_sleep,
    .wake = panel_wake, .write_area = write_area, .set_backlight = set_backlight, .set_rotation = set_rotation,
    .ctx = NULL};

/* ---------------------------------------------------------------- board */

static bool read_battery(void *ctx, uint8_t *pct, uint8_t *external)
{
    (void)ctx;
    *pct = sim.battery_pct;
    *external = sim.external_power;
    return sim.battery_readable;
}

static bool read_touch(void *ctx, board_touch_t *out)
{
    (void)ctx;
    *out = sim.touch;
    return true;
}

static void set_button_handler(void *ctx, board_button_cb cb, void *user)
{
    (void)ctx;
    sim.button_cb = cb;
    sim.button_user = user;
}

static board_profile_t profile = {
    .board_id = SIM_LCD_BOARD_ID,
    .flash_size = 16u * 1024u * 1024u,
    .features = PROTO_FEATURES_ANCS | PROTO_FEATURES_ANDROID_NOTIFICATIONS | PROTO_FEATURES_ASSETS |
                PROTO_FEATURES_OTA | PROTO_FEATURES_BATTERY | PROTO_FEATURES_BUTTONS | PROTO_FEATURES_TOUCH |
                PROTO_FEATURES_WIFI | PROTO_FEATURES_WEATHER, /* as the LCD firmware (D60) */
    .low_battery_ota_pct = 30,
    .display = &display,
    .read_battery = read_battery,
    .set_button_handler = set_button_handler,
    .read_touch = read_touch,
};

const board_profile_t *board_get(void) { return &profile; }

/* ---------------------------------------------------------------- simulator control */

void sim_lcd_reset(void)
{
    board_button_cb cb = sim.button_cb;
    void *user = sim.button_user;
    memset(&sim, 0, sizeof(sim));
    sim.battery_readable = true;
    sim.battery_pct = 80;
    sim.external_power = PROTO_EXTERNAL_POWER_NO;
    sim.button_cb = cb;
    sim.button_user = user;
    profile.read_battery = read_battery;
}

const uint16_t *sim_lcd_frame(void) { return sim.ram; }

void sim_lcd_panel(uint8_t *rgb)
{
    const uint32_t level = sim.asleep ? 0u : sim.backlight;
    for (uint32_t i = 0; i < N_PIXELS; i++) {
        const uint16_t p = sim.ram[sim.rotation == 2 ? N_PIXELS - 1u - i : i];
        const uint32_t r = (p >> 11) * 255u / 31u, g = ((p >> 5) & 0x3Fu) * 255u / 63u, b = (p & 0x1Fu) * 255u / 31u;
        rgb[3 * i] = (uint8_t)(r * level / 100u);
        rgb[3 * i + 1] = (uint8_t)(g * level / 100u);
        rgb[3 * i + 2] = (uint8_t)(b * level / 100u);
    }
}

void sim_lcd_state(sim_lcd_state_t *out)
{
    *out = (sim_lcd_state_t){.inited = sim.inited, .asleep = sim.asleep, .backlight = sim.backlight,
                             .writes = sim.writes, .pixels = sim.pixels, .last_x = sim.last_x,
                             .last_y = sim.last_y, .last_w = sim.last_w, .last_h = sim.last_h,
                             .violations = sim.violations, .rotation = sim.rotation};
}

void sim_lcd_clear_counters(void)
{
    sim.writes = 0;
    sim.pixels = 0;
}

void sim_lcd_press(board_button_t btn, board_press_t press)
{
    if (sim.button_cb) {
        sim.button_cb(sim.button_user, btn, press);
    }
}

void sim_lcd_touch(bool pressed, uint16_t x, uint16_t y)
{
    sim.touch = (board_touch_t){.pressed = pressed, .x = pressed ? x : 0, .y = pressed ? y : 0};
}

void sim_lcd_set_battery(bool reader, bool readable, uint8_t pct, uint8_t external_power)
{
    sim.battery_readable = readable;
    sim.battery_pct = pct;
    sim.external_power = external_power;
    profile.read_battery = reader ? read_battery : NULL;
}
