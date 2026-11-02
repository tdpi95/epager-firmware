/*
 * Area backend (LCD, PLAN D53): LVGL renders only the invalidated areas, in partial mode, into one
 * band buffer in RGB565 (CPU byte order, the area path's format); each area goes straight to the
 * driver's write_area, which copies it, so the buffer is free again at once. While the panel sleeps
 * (backlight off) nothing is written; ui.c invalidates the whole screen when it wakes.
 */
#include <string.h>

#include "ui_priv.h"

#define BAND_LINES 20u /* 296 x 20 x 2 = 11.8 KB from the LVGL heap */

static struct {
    uint8_t *buf;
} a;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const board_display_t *d = ui.p.display;
    if (!ui.panel_off) {
        d->write_area(d->ctx, (uint16_t)area->x1, (uint16_t)area->y1, (uint16_t)lv_area_get_width(area),
                      (uint16_t)lv_area_get_height(area), (const uint16_t *)px_map);
        if (lv_display_flush_is_last(disp)) {
            ui.shown_once = true; /* the backlight may come on: a real frame is on the panel */
        }
    }
    lv_display_flush_ready(disp);
}

static bool init(void)
{
    ui.disp = lv_display_create(ui.info.width, ui.info.height);
    lv_display_set_color_format(ui.disp, LV_COLOR_FORMAT_RGB565);
    const uint32_t len = ui.info.width * BAND_LINES * 2u;
    a.buf = lv_malloc(len);
    if (!a.buf) {
        return false;
    }
    lv_display_set_buffers(ui.disp, a.buf, NULL, len, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(ui.disp, flush_cb);
    return true;
}

/* Writes happen inside LVGL's refresh; only running animations keep the caller polling fast. */
static bool service(void) { return lv_anim_count_running() > 0; }

static void reset(void) { memset(&a, 0, sizeof(a)); }

const ui_backend_t ui_backend_area = {.init = init, .service = service, .reset = reset};
