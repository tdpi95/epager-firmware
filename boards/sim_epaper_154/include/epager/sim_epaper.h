/*
 * Simulated Waveshare ESP32-S3-ePaper-1.54 (host only). board_get() returns its profile; this
 * header is the simulator's control surface for the fake board, the viewer and golden tests.
 * Timing is virtual: the caller advances it with sim_epaper_tick(). Emulated (not measured):
 * partial refresh ~0.3 s, full refresh ~2 s with the black/white flash, optional ghosting. The RTC
 * counts with the virtual time; the temperature/humidity sensor returns what it was given.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epager/board_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_EPAPER_W 200u
#define SIM_EPAPER_H 200u
#define SIM_EPAPER_STRIDE ((SIM_EPAPER_W + 7u) / 8u)
#define SIM_EPAPER_FRAME (SIM_EPAPER_STRIDE * SIM_EPAPER_H)
#define SIM_EPAPER_PARTIAL_MS 300u
#define SIM_EPAPER_FULL_MS 2000u
#define SIM_EPAPER_BOARD_ID "sim_epaper_154"

typedef struct {
    bool refreshing, asleep, powered;
    uint8_t kind;            /* board_refresh_t of the current or last refresh */
    uint8_t phase;           /* full refresh: 0-3 (inverted old, black, white, inverted new) */
    uint32_t elapsed_ms;     /* into the current refresh */
    uint32_t partial, full;  /* refreshes completed */
    uint32_t violations;     /* writes or refreshes while busy, asleep or before init */
} sim_epaper_state_t;

/* Power-on state: blank (white) panel, uninitialised controller, default timing, no ghosting,
 * battery reader present at 80 %, no external power, RTC present but holding no time, sensor
 * present reading 24.50 degrees C and 50 %. Keeps the button handler. */
void sim_epaper_reset(void);
void sim_epaper_tick(uint32_t now_ms);
void sim_epaper_set_timing(uint32_t partial_ms, uint32_t full_ms);
void sim_epaper_set_ghosting(bool on);
/* What the panel shows now, one byte per pixel (0 black .. 255 white), w*h bytes. */
void sim_epaper_panel(uint8_t *gray);
/* The last completed frame (1-bit, board_api frame format). */
const uint8_t *sim_epaper_frame(void);
void sim_epaper_state(sim_epaper_state_t *out);
void sim_epaper_press(board_button_t btn, board_press_t press);
/* reader = false: the board has no battery reader (applies to users that read the profile after
 * this call, e.g. after a simulated reboot). readable = false: the reader fails. */
void sim_epaper_set_battery(bool reader, bool readable, uint8_t pct, uint8_t external_power);

/* RTC: present = false removes it from the profile (and PROTO_FEATURES_RTC); utc_s = 0: it holds no
 * valid time. Applies to users that read the profile after the call, like the battery reader. */
void sim_epaper_set_rtc(bool present, uint32_t utc_s);
uint32_t sim_epaper_rtc(void);  /* what the RTC reads now; 0 = absent or no valid time */
/* Temperature/humidity sensor: present = false removes it (and PROTO_FEATURES_CLIMATE);
 * readable = false: measurements fail. */
void sim_epaper_set_climate(bool present, bool readable, int16_t centi_c, uint16_t centi_rh);
uint32_t sim_epaper_climate_reads(void);

#ifdef __cplusplus
}
#endif
