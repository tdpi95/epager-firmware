/*
 * The board's I2C bus (ws154_pins.h) and the two chips the firmware uses on it: the NXP PCF85063A RTC
 * (schematic U8 "PCF85063ATL") and the Sensirion SHTC3 temperature/humidity sensor. Signatures match
 * board_profile_t. Not thread-safe: call from the one task that owns the board (the core/UI task).
 * The audio rail (it powers the ES8311, which would clamp the bus low) is switched on inside each call
 * and off again before it returns; ws154_i2c_init expects the pin to be configured as an output.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epager/board_api.h"

/* Create the bus and probe both chips. False only if the bus itself could not be created. */
bool ws154_i2c_init(void);
bool ws154_rtc_present(void);
bool ws154_climate_present(void);

bool ws154_rtc_get(void *ctx, uint32_t *utc_s);
bool ws154_rtc_set(void *ctx, uint32_t utc_s);
bool ws154_read_climate(void *ctx, board_climate_t *out);

/* The ES8311 on the same bus (ws154_audio.c): the audio rail on or off (the caller switches it on before the
 * first register write and off when done; settle time included), and one register write. */
void ws154_audio_rail(bool on);
/* The audio rail stays on between uses (a drain test, PLAN D106: does the unpowered codec clamp cost current?) or goes
 * back to being switched around each use. */
void ws154_i2c_hold_rail(bool hold);
bool ws154_codec_write(uint8_t reg, uint8_t value);
