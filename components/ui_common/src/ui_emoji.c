#include "ui_emoji.h"

bool ui_emoji_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *out, uint32_t letter, uint32_t letter_next)
{
    (void)font;
    (void)letter_next;
    uint32_t lo = 0, hi = ui_emoji_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (ui_emoji[mid].cp < letter) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == ui_emoji_count || ui_emoji[lo].cp != letter) {
        return false;
    }
    const lv_image_dsc_t *img = &ui_emoji_img[lo];
    out->adv_w = ui_emoji[lo].adv;
    out->box_w = img->header.w;
    out->box_h = img->header.h;
    out->ofs_x = ui_emoji[lo].ofs_x;
    out->ofs_y = ui_emoji[lo].ofs_y;
    out->format = LV_FONT_GLYPH_FORMAT_IMAGE;
    out->is_placeholder = 0;
    out->gid.src = img;
    return true;
}

/* An image glyph's "bitmap" is its image source; LVGL draws it with the image decoder. */
const void *ui_emoji_glyph_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf)
{
    (void)buf;
    return g->gid.src;
}
