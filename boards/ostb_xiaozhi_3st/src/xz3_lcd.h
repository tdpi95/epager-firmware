#pragma once

#include <stdbool.h>

#include "epager/board_api.h"

/* The LCD (area path of board_display_t); init happens in display->init. */
const board_display_t *xz3_lcd_get(void);
/* Turned by 180 degrees (display.orientation 2 or 3): the touch driver mirrors its points. */
bool xz3_lcd_flipped(void);
