/* Colour emoji for the LCD fonts (PLAN D59, private to ui_common): one RGB565A8 image per code point of the
 * registry's text.emoji_ranges, drawn by LVGL as image glyphs. The anti-aliased LCD fonts name
 * ui_font_emoji_colour as their fallback, so text measuring and wrapping see the emoji like any glyph.
 * Generated data: tools/assets/font_gen.py (fonts/ui_font_emoji_colour.c). */
#pragma once

#include <stdint.h>

#include "lvgl.h"

typedef struct {
    uint32_t cp;
    int8_t ofs_x, ofs_y; /* the image's place: right of the pen, bottom above the baseline (lv_font_glyph_dsc_t) */
    uint8_t adv;         /* advance, px */
} ui_emoji_t;

extern const ui_emoji_t ui_emoji[];          /* ascending code points */
extern const lv_image_dsc_t ui_emoji_img[];  /* same order */
extern const uint32_t ui_emoji_count;
extern const lv_font_t ui_font_emoji_colour;

/* lv_font_t callbacks of ui_font_emoji_colour */
bool ui_emoji_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *out, uint32_t letter, uint32_t letter_next);
const void *ui_emoji_glyph_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf);
