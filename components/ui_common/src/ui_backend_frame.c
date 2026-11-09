/*
 * Frame backend (e-paper, PLAN D29): LVGL renders the whole 1-bit screen (FULL mode) into its own
 * buffer; the flush keeps a packed copy (the shadow frame), which is pushed with write_frame + refresh
 * when the panel is not busy. Unchanged frames are not refreshed, the first refresh after boot is full,
 * then the board's choose_refresh decides (a full refresh every display.full_refresh_every partials, a clock
 * redraw counting as one and anything else as a quarter; one that falls due while the board is in use waits for
 * a quiet moment, at most until twice that many: PLAN D83), and the panel sleeps between refreshes.
 * display.orientation turns the picture in the shadow frame (clockwise quarter turns; a panel that is not
 * square shows the nearest of 0 and 180 degrees): the controller is not touched (PLAN D77).
 */
#include <string.h>

#include "ui_priv.h"

#define I1_PALETTE 8u /* LVGL reserves two palette entries at the start of an I1 buffer */

static struct {
    uint8_t *draw_buf, *shadow, *panel;
    uint32_t lv_stride, stride, frame_len;
    bool frame_ready, pushed_once, asleep;
    bool panel_wait; /* the last service left only the panel's refresh running */
    uint32_t quarters; /* partials since the last full refresh, a clock redraw counting 4 and anything else 1 */
} f;

/* Quarter turns actually shown for display.orientation. */
static uint8_t turns_shown(void)
{
    const uint8_t t = ui.cfg.orientation & 3u;
    return ui.info.width == ui.info.height ? t : (uint8_t)(t & 2u);
}

static bool bit(const uint8_t *rows, uint32_t stride, uint32_t x, uint32_t y)
{
    return (rows[y * stride + x / 8u] >> (7u - x % 8u)) & 1u;
}

/* The whole LVGL frame (rows of lv_stride bytes) into the shadow frame, turned clockwise by `turns` quarters. */
static void turn_into_shadow(const uint8_t *src, uint8_t turns)
{
    const uint32_t w = ui.info.width, h = ui.info.height;
    memset(f.shadow, 0, f.frame_len);
    for (uint32_t dy = 0; dy < h; dy++) {
        for (uint32_t dx = 0; dx < w; dx++) {
            uint32_t sx, sy;
            if (turns == 1) {
                sx = dy;
                sy = h - 1u - dx;
            } else if (turns == 2) {
                sx = w - 1u - dx;
                sy = h - 1u - dy;
            } else {
                sx = w - 1u - dy;
                sy = dx;
            }
            if (bit(src, f.lv_stride, sx, sy)) {
                f.shadow[dy * f.stride + dx / 8u] |= (uint8_t)(0x80u >> (dx % 8u));
            }
        }
    }
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    px_map += I1_PALETTE;
    const uint8_t turns = turns_shown();
    if (turns) { /* FULL render mode: the area is the whole screen */
        turn_into_shadow(px_map, turns);
    } else {
        for (int32_t y = area->y1; y <= area->y2; y++) {
            memcpy(f.shadow + (uint32_t)y * f.stride, px_map + (uint32_t)(y - area->y1) * f.lv_stride, f.stride);
        }
    }
    f.frame_ready = true;
    lv_display_flush_ready(disp);
}

static bool init(void)
{
    ui.disp = lv_display_create(ui.info.width, ui.info.height);
    lv_display_set_color_format(ui.disp, LV_COLOR_FORMAT_I1);
    f.stride = (ui.info.width + 7u) / 8u;
    f.lv_stride = lv_draw_buf_width_to_stride(ui.info.width, LV_COLOR_FORMAT_I1);
    f.frame_len = f.stride * ui.info.height;
    uint32_t buf_len = I1_PALETTE + f.lv_stride * ui.info.height;
    f.draw_buf = lv_malloc(buf_len);
    f.shadow = lv_malloc(f.frame_len);
    f.panel = lv_malloc(f.frame_len);
    if (!f.draw_buf || !f.shadow || !f.panel) {
        return false;
    }
    lv_display_set_buffers(ui.disp, f.draw_buf, NULL, buf_len, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(ui.disp, flush_cb);
    return true;
}

static bool service(void)
{
    const board_display_t *d = ui.p.display;
    bool busy = d->busy(d->ctx);
    if (f.frame_ready && !busy) {
        f.frame_ready = false;
        if (f.pushed_once && memcmp(f.shadow, f.panel, f.frame_len) == 0) {
            ui.stats.skipped++;
        } else {
            /* the panel's state is unknown at boot: start with a full refresh */
            const uint32_t every = ui.cfg.full_refresh_every ? ui.cfg.full_refresh_every : 1u;
            const uint32_t n = f.quarters / 4u;
            board_refresh_t kind = !f.pushed_once ? BOARD_REFRESH_FULL : d->choose_refresh(d->ctx, n, every);
            if (kind == BOARD_REFRESH_FULL && f.pushed_once && ui_in_use() && n < 2u * every) {
                kind = BOARD_REFRESH_PARTIAL; /* the flashing waits for a quiet moment, but not for ever (PLAN D83) */
            }
            if (f.asleep) {
                d->wake(d->ctx);
                f.asleep = false;
            }
            d->write_frame(d->ctx, f.shadow);
            d->refresh(d->ctx, kind);
            memcpy(f.panel, f.shadow, f.frame_len);
            f.pushed_once = true;
            ui.stats.last_kind = (uint8_t)kind;
            if (kind == BOARD_REFRESH_FULL) {
                ui.stats.full++;
                f.quarters = 0;
            } else {
                ui.stats.partial++;
                f.quarters += ui_clock_shown() ? 4u : 1u;
            }
            ui.stats.partials_since_full = f.quarters / 4u;
            busy = true;
        }
    }
    if (!busy && !f.frame_ready && f.pushed_once && !f.asleep) {
        d->sleep(d->ctx);
        f.asleep = true;
    }
    f.panel_wait = busy && !f.frame_ready;
    return busy || f.frame_ready;
}

/* Only the panel is working: the next frame (if any) can wait for the end of its refresh. */
static bool panel_wait(void) { return f.panel_wait; }

static void reset(void) { memset(&f, 0, sizeof(f)); }

const ui_backend_t ui_backend_frame = {.init = init, .service = service, .panel_wait = panel_wait, .reset = reset};
