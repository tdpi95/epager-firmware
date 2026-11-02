/*
 * 200x200 1-bit e-paper driver of the Waveshare ESP32-S3-ePaper-1.54 (SSD1681-class controller,
 * implements board_display_t). Private to the board: the shared code only sees board_api.h.
 */
#pragma once

#include "epager/board_api.h"

const board_display_t *ws154_epd_get(void);
