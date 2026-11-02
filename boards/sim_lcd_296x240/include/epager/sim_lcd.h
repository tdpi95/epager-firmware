/*
 * Simulated LCD board (host only, PLAN D56): the ostb-xiaozhi-3st's 296 x 240 RGB565 LCD on the
 * area path of board_api, its backlight, CST816S-style polled touch, the volume keys and the battery.
 * board_get() returns its profile; this header is the simulator's control surface for the fake board,
 * the viewer and the tests. No RTC and no climate sensor, like the real board.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epager/board_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_LCD_W 296u
#define SIM_LCD_H 240u
#define SIM_LCD_EDGE_INSET 14u /* as the LCD board: rounded corners */
#define SIM_LCD_BOARD_ID "sim_lcd_296x240"

typedef struct {
    bool inited, asleep;
    uint8_t backlight;        /* percent, as last set */
    uint32_t writes;          /* write_area calls that drew something */
    uint64_t pixels;          /* pixels written by them */
    uint16_t last_x, last_y, last_w, last_h; /* the last area written (after clipping) */
    uint32_t violations;      /* writes before init or while asleep */
    uint8_t rotation;         /* quarter turns shown: 0 or 2 (like the real board) */
} sim_lcd_state_t;

/* Power-on state: black panel RAM, uninitialised controller, backlight off, no touch, battery reader
 * present at 80 %, no external power. Keeps the button handler. */
void sim_lcd_reset(void);
/* The panel RAM (RGB565, CPU order), w*h pixels: what the UI drew, whatever the backlight. */
const uint16_t *sim_lcd_frame(void);
/* What a person would see: the RAM in 8-bit RGB scaled by the backlight, black while the panel
 * sleeps, turned by 180 degrees when the rotation is 2; w*h*3 bytes. */
void sim_lcd_panel(uint8_t *rgb);
void sim_lcd_state(sim_lcd_state_t *out);
/* Clear the write counters (tests measure what one change sends). */
void sim_lcd_clear_counters(void);
void sim_lcd_press(board_button_t btn, board_press_t press);
/* The touch point read_touch reports from now on (display coordinates). */
void sim_lcd_touch(bool pressed, uint16_t x, uint16_t y);
/* reader = false: no battery reader (for users that read the profile after the call). */
void sim_lcd_set_battery(bool reader, bool readable, uint8_t pct, uint8_t external_power);

#ifdef __cplusplus
}
#endif
