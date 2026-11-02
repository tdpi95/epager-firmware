#include "epager/sim_epaper.h"

#include <string.h>

#include "epager/board_util.h"
#include "epager/proto_gen.h"

/* Grey levels the viewer shows for ghosting (a pixel last changed by a partial refresh keeps a
 * trace of its old colour) and for pixels in transition during a partial refresh. */
#define GHOST_ON_WHITE 220u
#define GHOST_ON_BLACK 40u
#define TRANSITION 128u

static struct {
    uint32_t now, start;
    uint32_t partial_ms, full_ms;
    bool inited, asleep, refreshing, ghosting, powered;
    board_refresh_t kind;
    uint8_t ram[SIM_EPAPER_FRAME];    /* controller memory: the next frame */
    uint8_t old[SIM_EPAPER_FRAME];    /* frame shown before the current refresh */
    uint8_t shown[SIM_EPAPER_FRAME];  /* last completed frame */
    uint8_t ghost[SIM_EPAPER_FRAME];  /* 1 = pixel changed by a partial refresh since the last full */
    uint32_t partial, full, violations;
    bool battery_reader, battery_readable;
    uint8_t battery_pct, external_power;
    bool rtc_present, climate_present, climate_readable;
    uint32_t rtc_utc, rtc_at_ms; /* the RTC counts from rtc_utc since sim.now == rtc_at_ms */
    board_climate_t climate;
    uint32_t climate_reads;
    board_button_cb button_cb;
    void *button_user;
} sim;

static inline bool bit(const uint8_t *f, uint32_t x, uint32_t y)
{
    return (f[y * SIM_EPAPER_STRIDE + x / 8u] >> (7u - x % 8u)) & 1u;
}

/* ---------------------------------------------------------------- display driver */

static void get_info(void *ctx, board_display_info_t *out)
{
    (void)ctx;
    *out = (board_display_info_t){.width = SIM_EPAPER_W, .height = SIM_EPAPER_H,
                                  .display_type = PROTO_DISPLAY_TYPE_EPAPER_BW, .color_depth = 1,
                                  .refresh_class = PROTO_REFRESH_CLASS_EPAPER};
}

static bool init(void *ctx)
{
    (void)ctx;
    sim.inited = true;
    sim.asleep = false;
    sim.powered = true;
    return true;
}

static bool busy(void *ctx)
{
    (void)ctx;
    return sim.refreshing;
}

static void write_frame(void *ctx, const uint8_t *frame)
{
    (void)ctx;
    if (!sim.inited || sim.asleep || sim.refreshing) {
        sim.violations++;
        return;
    }
    memcpy(sim.ram, frame, SIM_EPAPER_FRAME);
}

static void refresh(void *ctx, board_refresh_t kind)
{
    (void)ctx;
    if (!sim.inited || sim.asleep || sim.refreshing) {
        sim.violations++;
        return;
    }
    memcpy(sim.old, sim.shown, SIM_EPAPER_FRAME);
    sim.kind = kind;
    sim.start = sim.now;
    sim.refreshing = true;
}

static board_refresh_t choose_refresh(void *ctx, uint32_t partials_since_full, uint32_t full_every)
{
    (void)ctx;
    return partials_since_full >= (full_every ? full_every : 1u) ? BOARD_REFRESH_FULL : BOARD_REFRESH_PARTIAL;
}

static void panel_sleep(void *ctx)
{
    (void)ctx;
    if (sim.refreshing) {
        sim.violations++;
    }
    sim.asleep = true;
}

static void panel_wake(void *ctx)
{
    (void)ctx;
    sim.asleep = false;
}

static const board_display_t display = {
    .get_info = get_info, .init = init, .write_frame = write_frame, .refresh = refresh, .busy = busy,
    .choose_refresh = choose_refresh, .sleep = panel_sleep, .wake = panel_wake, .ctx = NULL};

/* ---------------------------------------------------------------- board */

static bool read_battery(void *ctx, uint8_t *pct, uint8_t *external)
{
    (void)ctx;
    *pct = sim.battery_pct;
    *external = sim.external_power;
    return sim.battery_readable;
}

static bool rtc_get(void *ctx, uint32_t *utc)
{
    (void)ctx;
    *utc = sim_epaper_rtc();
    return *utc != 0;
}

static bool rtc_set(void *ctx, uint32_t utc)
{
    (void)ctx;
    if (utc < BOARD_RTC_MIN_UTC || utc > BOARD_RTC_MAX_UTC) {
        return false; /* what the PCF85063 can hold */
    }
    sim.rtc_utc = utc;
    sim.rtc_at_ms = sim.now;
    return true;
}

static bool read_climate(void *ctx, board_climate_t *out)
{
    (void)ctx;
    sim.climate_reads++;
    *out = sim.climate;
    return sim.climate_readable;
}

static void set_button_handler(void *ctx, board_button_cb cb, void *user)
{
    (void)ctx;
    sim.button_cb = cb;
    sim.button_user = user;
}

static board_profile_t profile = {
    .board_id = SIM_EPAPER_BOARD_ID,
    .flash_size = 8u * 1024u * 1024u,
    .features = PROTO_FEATURES_ANCS | PROTO_FEATURES_ANDROID_NOTIFICATIONS | PROTO_FEATURES_ASSETS |
                PROTO_FEATURES_OTA | PROTO_FEATURES_BATTERY | PROTO_FEATURES_BUTTONS | PROTO_FEATURES_RTC |
                PROTO_FEATURES_CLIMATE | PROTO_FEATURES_WIFI | PROTO_FEATURES_WEATHER, /* as the Paper firmware (D78) */
    .low_battery_ota_pct = 30,
    .display = &display,
    .read_battery = read_battery,
    .set_button_handler = set_button_handler,
    .rtc_get = rtc_get,
    .rtc_set = rtc_set,
    .read_climate = read_climate,
};

static void apply_presence(void)
{
    const uint32_t opt = PROTO_FEATURES_RTC | PROTO_FEATURES_CLIMATE;
    profile.features = (profile.features & ~opt) | (sim.rtc_present ? PROTO_FEATURES_RTC : 0u) |
                       (sim.climate_present ? PROTO_FEATURES_CLIMATE : 0u);
    profile.rtc_get = sim.rtc_present ? rtc_get : NULL;
    profile.rtc_set = sim.rtc_present ? rtc_set : NULL;
    profile.read_climate = sim.climate_present ? read_climate : NULL;
}

const board_profile_t *board_get(void) { return &profile; }

/* ---------------------------------------------------------------- simulator control */

void sim_epaper_reset(void)
{
    board_button_cb cb = sim.button_cb;
    void *user = sim.button_user;
    memset(&sim, 0, sizeof(sim));
    memset(sim.ram, 0xFF, sizeof(sim.ram));
    memset(sim.shown, 0xFF, sizeof(sim.shown));
    sim.partial_ms = SIM_EPAPER_PARTIAL_MS;
    sim.full_ms = SIM_EPAPER_FULL_MS;
    sim.battery_reader = sim.battery_readable = true;
    sim.battery_pct = 80;
    sim.external_power = PROTO_EXTERNAL_POWER_NO;
    sim.button_cb = cb;
    sim.button_user = user;
    profile.read_battery = read_battery;
    sim.rtc_present = true; /* holding no time: the RTC loses it with the power */
    sim.climate_present = sim.climate_readable = true;
    sim.climate = (board_climate_t){.temp_centi_c = 2450, .rh_centi_pct = 5000};
    apply_presence();
}

void sim_epaper_tick(uint32_t now_ms)
{
    sim.now = now_ms;
    if (!sim.refreshing || now_ms - sim.start < (sim.kind == BOARD_REFRESH_FULL ? sim.full_ms : sim.partial_ms)) {
        return;
    }
    sim.refreshing = false;
    for (uint32_t i = 0; i < SIM_EPAPER_FRAME; i++) {
        uint8_t changed = (uint8_t)(sim.old[i] ^ sim.ram[i]);
        sim.ghost[i] = sim.kind == BOARD_REFRESH_FULL ? 0u : (uint8_t)(sim.ghost[i] | changed);
    }
    memcpy(sim.shown, sim.ram, SIM_EPAPER_FRAME);
    if (sim.kind == BOARD_REFRESH_FULL) {
        sim.full++;
    } else {
        sim.partial++;
    }
}

void sim_epaper_set_timing(uint32_t partial_ms, uint32_t full_ms)
{
    sim.partial_ms = partial_ms;
    sim.full_ms = full_ms;
}

void sim_epaper_set_ghosting(bool on) { sim.ghosting = on; }

static uint8_t full_phase(void)
{
    uint32_t q = sim.full_ms / 4u;
    uint32_t e = sim.now - sim.start;
    return (uint8_t)(q ? (e / q > 3u ? 3u : e / q) : 3u);
}

void sim_epaper_panel(uint8_t *gray)
{
    uint8_t phase = sim.refreshing && sim.kind == BOARD_REFRESH_FULL ? full_phase() : 0;
    for (uint32_t y = 0; y < SIM_EPAPER_H; y++) {
        for (uint32_t x = 0; x < SIM_EPAPER_W; x++) {
            bool white = bit(sim.shown, x, y);
            uint8_t v = white ? 255u : 0u;
            if (sim.refreshing && sim.kind == BOARD_REFRESH_FULL) {
                v = phase == 0 ? (bit(sim.old, x, y) ? 0u : 255u)
                  : phase == 1 ? 0u
                  : phase == 2 ? 255u
                               : (bit(sim.ram, x, y) ? 0u : 255u);
            } else if (sim.refreshing && bit(sim.old, x, y) != bit(sim.ram, x, y)) {
                v = TRANSITION;
            } else if (sim.ghosting && bit(sim.ghost, x, y)) {
                v = white ? GHOST_ON_WHITE : GHOST_ON_BLACK;
            }
            gray[y * SIM_EPAPER_W + x] = v;
        }
    }
}

const uint8_t *sim_epaper_frame(void) { return sim.shown; }

void sim_epaper_state(sim_epaper_state_t *out)
{
    *out = (sim_epaper_state_t){.refreshing = sim.refreshing, .asleep = sim.asleep, .powered = sim.powered,
                                .kind = (uint8_t)sim.kind,
                                .phase = sim.refreshing && sim.kind == BOARD_REFRESH_FULL ? full_phase() : 0,
                                .elapsed_ms = sim.refreshing ? sim.now - sim.start : 0, .partial = sim.partial,
                                .full = sim.full, .violations = sim.violations};
}

void sim_epaper_press(board_button_t btn, board_press_t press)
{
    if (sim.button_cb) {
        sim.button_cb(sim.button_user, btn, press);
    }
}

void sim_epaper_set_battery(bool reader, bool readable, uint8_t pct, uint8_t external_power)
{
    sim.battery_reader = reader;
    sim.battery_readable = readable;
    sim.battery_pct = pct;
    sim.external_power = external_power;
    profile.read_battery = reader ? read_battery : NULL;
}

void sim_epaper_set_rtc(bool present, uint32_t utc_s)
{
    sim.rtc_present = present;
    sim.rtc_utc = utc_s;
    sim.rtc_at_ms = sim.now;
    apply_presence();
}

uint32_t sim_epaper_rtc(void)
{
    return sim.rtc_present && sim.rtc_utc ? sim.rtc_utc + (sim.now - sim.rtc_at_ms) / 1000u : 0;
}

void sim_epaper_set_climate(bool present, bool readable, int16_t centi_c, uint16_t centi_rh)
{
    sim.climate_present = present;
    sim.climate_readable = readable;
    sim.climate = (board_climate_t){.temp_centi_c = centi_c, .rh_centi_pct = centi_rh};
    apply_presence();
}

uint32_t sim_epaper_climate_reads(void) { return sim.climate_reads; }
