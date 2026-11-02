#pragma once

#include <stdbool.h>

#include "epager/board_api.h"

/* The shared I2C bus (touch, codec, microphone ADC) and the CST816S on it. */
bool xz3_i2c_init(void);
/* board_profile_t.read_touch */
bool xz3_touch_read(void *ctx, board_touch_t *out);
