/*
 * Clock faces on the idle screen (contracts/asset-formats.md "Clock faces", PLAN D113): the renderer. The portable
 * component clock_face parses a face file and says what each element draws (cf_eval); this file turns that into LVGL
 * objects, once per face, and updates only what changed. Classic (the layouts' own idle screen) is the fallback for
 * every failure: no face selected, not installed, made for another screen size, damaged, or too big for memory.
 *
 * The face is read into a temporary buffer, parsed, and its images are copied into aligned buffers owned here; the
 * buffer is freed again, so a face costs its pixels once (plus the objects).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "epager/blob_storage.h"
#include "epager/clock_face.h"
#include "epager/proto_gen.h"
#include "epager/xfer.h"
#include "ui_face_builtin.h"
#include "ui_icons.h"
#include "ui_priv.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#if UI_HAS_MONO
LV_FONT_DECLARE(ui_font_regular_16)
LV_FONT_DECLARE(ui_font_bold_16)
LV_FONT_DECLARE(ui_font_clock_56)
LV_FONT_DECLARE(ui_font_bold_24)
#endif
#if UI_HAS_COLOUR
LV_FONT_DECLARE(ui_font_lcd_16)
LV_FONT_DECLARE(ui_font_lcd_bold_20)
LV_FONT_DECLARE(ui_font_lcd_clock_72)
#endif

#define FILE_MAX_MONO (64u * 1024u)
#define FILE_MAX_COLOUR (1024u * 1024u)
#define OBJECTS_MAX 220u         /* LVGL objects one face may need; more falls back to Classic */
#define OBJECT_BYTES 320u        /* what an object costs in LVGL's pool, with its styles (conservative) */
#define MARK_SIZE 22
#define MARK_MARGIN 4
#define ICON_WX 32
#define ICON_STATUS 16

typedef struct {
    uint8_t *mem;                /* the pixels, outside LVGL's pool (PSRAM on the boards) */
    lv_image_dsc_t dsc;
    const void *src;             /* what lv_image_set_src takes */
    bool mask;                   /* drawn in the element's colour */
    uint16_t w, h;
} img_t;

typedef struct {
    lv_obj_t *obj;               /* rect, line, arc, image, text, icon; NULL for number (glyphs) and unknown kinds */
    lv_obj_t *glyph[CF_GLYPH_MAX];
    uint8_t n_glyph;             /* glyph objects created */
    lv_point_precise_t pts[2];   /* diagonal lines */
    int shown;                   /* icon: what the image shows (a key), 0 = nothing yet */
    int shown_value;             /* arc: the value drawn */
    uint16_t colour;
    bool colour_valid;
    char number[24];             /* number: the text it shows (for ui_visible_text) */
} el_t;

static struct {
    bool loaded, active, builtin;
    char id[CF_NAME_MAX + 1];    /* the id this state was loaded for ("" none) */
    uint32_t gen;                /* ui.assets_gen at load */
    uint16_t w, h;
    uint32_t errors;
    uint8_t opts[32], opts_len;  /* ui.clock_face_opts at load */
    uint8_t values[CF_MAX_OPTIONS];
    lv_obj_t *root, *badge, *mark;
    lv_draw_buf_t *wx_icon[UI_WEATHER_ICON_COUNT];
    lv_draw_buf_t *status_icon[UI_STATUS_ICON_COUNT];
    bool update_drawn;           /* the face draws update_available itself */
} fc;

static UI_EXT_BSS cf_face_t face;
static UI_EXT_BSS img_t images[CF_MAX_IMAGES];
static UI_EXT_BSS el_t els[CF_MAX_ELEMENTS];

/* ---------------------------------------------------------------- helpers */

static void *big_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}

static bool colour_layout(void) { return ui.info.color_depth != 1; }

static const lv_font_t *font_for(uint8_t role)
{
#if UI_HAS_COLOUR
    if (colour_layout()) {
        switch (role) {
        case PROTO_FACE_FONT_SMALL: return &ui_font_lcd_16;
        case PROTO_FACE_FONT_SMALL_BOLD:
        case PROTO_FACE_FONT_LARGE_BOLD: return &ui_font_lcd_bold_20;
        case PROTO_FACE_FONT_CLOCK: return &ui_font_lcd_clock_72;
        default: return NULL;
        }
    }
#endif
#if UI_HAS_MONO
    if (!colour_layout()) {
        switch (role) {
        case PROTO_FACE_FONT_SMALL: return &ui_font_regular_16;
        case PROTO_FACE_FONT_SMALL_BOLD: return &ui_font_bold_16;
        case PROTO_FACE_FONT_LARGE_BOLD: return &ui_font_bold_24;
        case PROTO_FACE_FONT_CLOCK: return &ui_font_clock_56;
        default: return NULL;
        }
    }
#endif
    return NULL;
}

/* RGB565 to a colour; on a 1-bit display white when the luma is at least half, else black. */
static lv_color_t colour_of(uint16_t c)
{
    const unsigned r5 = c >> 11, g6 = (c >> 5) & 63u, b5 = c & 31u;
    const unsigned r = r5 << 3 | r5 >> 2, g = g6 << 2 | g6 >> 4, b = b5 << 3 | b5 >> 2;
    if (!colour_layout()) {
        return 299u * r + 587u * g + 114u * b >= 127500u ? lv_color_white() : lv_color_black();
    }
    return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static void plain(lv_obj_t *o)
{
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
}

/* An A1 mask (bit 1 = ink) from a mono1 bitmap (bit 1 = white): ink is the black pixels. */
static lv_draw_buf_t *mask_from_mono1(const uint8_t *src, uint32_t w, uint32_t h)
{
    lv_draw_buf_t *b = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_A1, 0);
    if (!b) {
        return NULL;
    }
    const uint32_t row_bytes = (w + 7u) / 8u;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = lv_draw_buf_goto_xy(b, 0, y);
        for (uint32_t i = 0; i < row_bytes; i++) {
            row[i] = (uint8_t)~src[y * row_bytes + i];
        }
    }
    return b;
}

/* ---------------------------------------------------------------- images */

static void free_images(void)
{
    for (unsigned i = 0; i < CF_MAX_IMAGES; i++) {
        free(images[i].mem);
    }
    memset(images, 0, sizeof(images));
}

static bool prepare_images(void)
{
    for (unsigned i = 0; i < face.n_images; i++) {
        const cf_image_t *im = &face.image[i];
        img_t *m = &images[i];
        m->w = im->width;
        m->h = im->height;
        if (im->format == PROTO_ASSET_FORMAT_MONO1) { /* an A1 mask: ink (bit 0 in the file) becomes bit 1 */
            const uint32_t stride = lv_draw_buf_width_to_stride(im->width, LV_COLOR_FORMAT_A1);
            const uint32_t row_bytes = ((uint32_t)im->width + 7u) / 8u;
            m->mem = big_alloc((size_t)stride * im->height);
            if (!m->mem) {
                return false;
            }
            memset(m->mem, 0, (size_t)stride * im->height);
            for (uint32_t y = 0; y < im->height; y++) {
                for (uint32_t k = 0; k < row_bytes; k++) {
                    m->mem[y * stride + k] = (uint8_t)~im->data[y * row_bytes + k];
                }
            }
            m->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            m->dsc.header.cf = LV_COLOR_FORMAT_A1;
            m->dsc.header.w = im->width;
            m->dsc.header.h = im->height;
            m->dsc.header.stride = stride;
            m->dsc.data_size = (uint32_t)(stride * im->height);
            m->dsc.data = m->mem;
            m->src = &m->dsc;
            m->mask = true;
            continue;
        }
        m->mem = big_alloc(im->len);
        if (!m->mem) {
            return false;
        }
        memcpy(m->mem, im->data, im->len);
        m->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        m->dsc.header.cf = im->format == PROTO_ASSET_FORMAT_RGB565 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB565A8;
        m->dsc.header.w = im->width;
        m->dsc.header.h = im->height;
        m->dsc.header.stride = (uint32_t)im->width * 2u;
        m->dsc.data_size = im->len;
        m->dsc.data = m->mem;
        m->src = &m->dsc;
    }
    return true;
}

/* ---------------------------------------------------------------- objects */

static unsigned glyph_cap(const cf_element_t *e)
{
    switch (e->source) {
    case PROTO_FACE_SOURCE_AMPM: return 1;
    case PROTO_FACE_SOURCE_HOUR: case PROTO_FACE_SOURCE_MINUTE: case PROTO_FACE_SOURCE_DAY:
    case PROTO_FACE_SOURCE_MONTH: return 2;
    case PROTO_FACE_SOURCE_YEAR: return 4;
    case PROTO_FACE_SOURCE_TIME: return 5;
    case PROTO_FACE_SOURCE_WEATHER_TEMP: case PROTO_FACE_SOURCE_WEATHER_HIGH: case PROTO_FACE_SOURCE_WEATHER_LOW:
    case PROTO_FACE_SOURCE_WEATHER_RAIN: case PROTO_FACE_SOURCE_ROOM_TEMP: case PROTO_FACE_SOURCE_ROOM_HUMIDITY:
    case PROTO_FACE_SOURCE_BATTERY_PCT: case PROTO_FACE_SOURCE_NOTIF_COUNT: return 6;
    default: return 8;
    }
}

static unsigned object_estimate(void)
{
    unsigned n = 4; /* root, badge, mark, spare */
    for (unsigned i = 0; i < face.n_elements; i++) {
        const cf_element_t *e = &face.element[i];
        switch (e->kind) {
        case PROTO_FACE_ELEMENT_RECT: case PROTO_FACE_ELEMENT_LINE: case PROTO_FACE_ELEMENT_ARC:
        case PROTO_FACE_ELEMENT_IMAGE: case PROTO_FACE_ELEMENT_TEXT: case PROTO_FACE_ELEMENT_ICON: n++; break;
        case PROTO_FACE_ELEMENT_NUMBER: n += glyph_cap(e); break;
        default: break;
        }
    }
    return n;
}

static lv_obj_t *new_image(lv_obj_t *parent)
{
    lv_obj_t *o = lv_image_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static void build_element(const cf_element_t *e, el_t *x)
{
    lv_obj_t *root = fc.root;
    switch (e->kind) {
    case PROTO_FACE_ELEMENT_RECT:
        x->obj = lv_obj_create(root);
        plain(x->obj);
        lv_obj_set_size(x->obj, e->w, e->h);
        lv_obj_set_pos(x->obj, e->x, e->y);
        lv_obj_set_style_radius(x->obj, e->radius, 0);
        if (e->fill) {
            lv_obj_set_style_bg_opa(x->obj, LV_OPA_COVER, 0);
        }
        lv_obj_set_style_border_width(x->obj, e->thickness, 0);
        break;
    case PROTO_FACE_ELEMENT_LINE: {
        const uint8_t t = e->thickness ? e->thickness : 1;
        if (e->w == 0 || e->h == 0) { /* horizontal or vertical: a thin box */
            x->obj = lv_obj_create(root);
            plain(x->obj);
            lv_obj_set_style_bg_opa(x->obj, LV_OPA_COVER, 0);
            lv_obj_set_pos(x->obj, e->x, e->y);
            lv_obj_set_size(x->obj, e->w ? e->w : t, e->h ? e->h : t);
        } else {
            x->pts[0] = (lv_point_precise_t){0, 0};
            x->pts[1] = (lv_point_precise_t){e->w, e->h};
            x->obj = lv_line_create(root);
            lv_obj_remove_flag(x->obj, LV_OBJ_FLAG_CLICKABLE);
            lv_line_set_points(x->obj, x->pts, 2);
            lv_obj_set_pos(x->obj, e->x, e->y);
            lv_obj_set_style_line_width(x->obj, t, 0);
        }
        break;
    }
    case PROTO_FACE_ELEMENT_ARC: {
        const uint8_t t = e->thickness ? e->thickness : 1;
        x->obj = lv_arc_create(root);
        lv_obj_remove_flag(x->obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(x->obj, e->w, e->h);
        lv_obj_set_pos(x->obj, e->x, e->y);
        lv_arc_set_rotation(x->obj, 0);
        lv_arc_set_bg_angles(x->obj, e->arc_start, e->arc_end);
        lv_arc_set_range(x->obj, 0, 100);
        lv_obj_set_style_arc_width(x->obj, t, LV_PART_MAIN);
        lv_obj_set_style_arc_width(x->obj, t, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(x->obj, LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_opa(x->obj, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(x->obj, 0, LV_PART_KNOB);
        lv_obj_set_style_bg_opa(x->obj, LV_OPA_TRANSP, LV_PART_KNOB);
        x->shown_value = -1;
        break;
    }
    case PROTO_FACE_ELEMENT_IMAGE:
        x->obj = new_image(root);
        lv_image_set_src(x->obj, images[e->image].src);
        lv_obj_set_pos(x->obj, e->x, e->y);
        break;
    case PROTO_FACE_ELEMENT_NUMBER:
        x->n_glyph = (uint8_t)glyph_cap(e);
        for (unsigned g = 0; g < x->n_glyph; g++) {
            x->glyph[g] = new_image(root);
        }
        break;
    case PROTO_FACE_ELEMENT_TEXT: {
        const lv_font_t *f = font_for(e->font);
        if (!f) {
            break; /* a font this firmware does not have: the element is skipped */
        }
        x->obj = lv_label_create(root);
        lv_obj_remove_flag(x->obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(x->obj, e->x, e->y);
        lv_obj_set_size(x->obj, e->w, e->h);
        lv_label_set_long_mode(x->obj, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_font(x->obj, f, 0);
        lv_obj_set_style_text_align(x->obj, e->align == PROTO_FACE_ALIGN_CENTER ? LV_TEXT_ALIGN_CENTER
                                           : e->align == PROTO_FACE_ALIGN_RIGHT ? LV_TEXT_ALIGN_RIGHT
                                                                                 : LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text_static(x->obj, "");
        break;
    }
    case PROTO_FACE_ELEMENT_ICON:
        x->obj = new_image(root);
        break;
    default: break;
    }
    if (x->obj) {
        lv_obj_add_flag(x->obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_extras(void)
{
    const bool colour = colour_layout();
    const lv_font_t *f = font_for(PROTO_FACE_FONT_SMALL_BOLD);
    const int32_t bh = colour ? 24 : 22, bw = colour ? 30 : 30;
    fc.badge = lv_label_create(fc.root);
    lv_obj_remove_flag(fc.badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(fc.badge, bw, bh);
    lv_obj_set_pos(fc.badge, colour ? 8 : 6, (int32_t)fc.h - bh - (colour ? 8 : 2));
    lv_label_set_long_mode(fc.badge, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(fc.badge, f, 0);
    lv_obj_set_style_text_color(fc.badge, lv_color_white(), 0);
    lv_obj_set_style_text_align(fc.badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(fc.badge, colour ? lv_color_make(0x3B, 0x82, 0xF6) : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(fc.badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fc.badge, bh / 2, 0);
    lv_obj_set_style_pad_top(fc.badge, (bh - (int32_t)f->line_height) / 2, 0);
    lv_label_set_text_static(fc.badge, "");
    lv_obj_add_flag(fc.badge, LV_OBJ_FLAG_HIDDEN);

    fc.mark = lv_label_create(fc.root);
    lv_obj_remove_flag(fc.mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(fc.mark, MARK_SIZE, MARK_SIZE);
    int32_t mx = (int32_t)fc.w - MARK_SIZE - MARK_MARGIN, my = MARK_MARGIN;
    switch (face.update_anchor) {
    case PROTO_FACE_ANCHOR_TOP_LEFT: mx = MARK_MARGIN; break;
    case PROTO_FACE_ANCHOR_BOTTOM_RIGHT: my = (int32_t)fc.h - MARK_SIZE - MARK_MARGIN; break;
    case PROTO_FACE_ANCHOR_BOTTOM_LEFT: mx = MARK_MARGIN; my = (int32_t)fc.h - MARK_SIZE - MARK_MARGIN; break;
    default: break;
    }
    lv_obj_set_pos(fc.mark, mx, my);
    lv_label_set_long_mode(fc.mark, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(fc.mark, f, 0);
    lv_obj_set_style_text_color(fc.mark, colour ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_text_align(fc.mark, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(fc.mark, colour ? lv_color_make(0xF5, 0x9E, 0x0B) : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(fc.mark, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fc.mark, MARK_SIZE / 2, 0);
    lv_obj_set_style_pad_top(fc.mark, (MARK_SIZE - (int32_t)f->line_height) / 2, 0);
    lv_label_set_text_static(fc.mark, "!");
    lv_obj_add_flag(fc.mark, LV_OBJ_FLAG_HIDDEN);
}

static bool build_objects(void)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    const unsigned n = object_estimate();
    if (n > OBJECTS_MAX || (size_t)n * OBJECT_BYTES + 4096u > mon.free_size) {
        return false;
    }
    fc.root = lv_obj_create(lv_screen_active());
    plain(fc.root);
    lv_obj_set_pos(fc.root, 0, 0);
    lv_obj_set_size(fc.root, fc.w, fc.h);
    lv_obj_move_background(fc.root); /* under the layout's own objects: a message or the power-off text stays on top */
    lv_obj_add_flag(fc.root, LV_OBJ_FLAG_HIDDEN);
    for (unsigned i = 0; i < face.n_elements; i++) {
        build_element(&face.element[i], &els[i]);
    }
    build_extras();
    return true;
}

static void free_face(void)
{
    if (fc.root) {
        lv_obj_delete(fc.root); /* its children go with it */
    }
    free_images();
    memset(els, 0, sizeof(els));
    fc.root = fc.badge = fc.mark = NULL;
    fc.active = false;
    fc.builtin = false;
}

/* ---------------------------------------------------------------- loading */

static const ui_builtin_face_t *find_builtin(const char *id)
{
    const ui_builtin_face_t *list = NULL;
    size_t n = 0;
#if UI_HAS_COLOUR
    if (colour_layout()) {
        list = ui_builtin_faces_colour;
        n = ui_builtin_faces_colour_count;
    }
#endif
#if UI_HAS_MONO
    if (!colour_layout()) {
        list = ui_builtin_faces_mono;
        n = ui_builtin_faces_mono_count;
    }
#endif
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i].id, id) == 0) {
            return &list[i];
        }
    }
    return NULL;
}

/* The face file of an installed face in a new buffer (caller frees), or NULL. */
static uint8_t *read_installed(const char *id, size_t max, size_t *len)
{
    const blob_storage_t *b = ui.p.blob;
    char name[BLOB_NAME_MAX + 1];
    uint32_t size = 0;
    if (!b) {
        return NULL;
    }
    xfer_asset_blob_name(PROTO_ASSET_TYPE_CLOCK_FACE, id, strlen(id), name);
    if (b->stat(b->ctx, name, &size) != BLOB_OK || size == 0 || size > max) {
        return NULL;
    }
    uint8_t *buf = big_alloc(size);
    if (buf && b->read(b->ctx, name, 0, buf, size) != BLOB_OK) {
        free(buf);
        buf = NULL;
    }
    *len = size;
    return buf;
}

static cf_limits_t limits_for(uint16_t w, uint16_t h, bool colour)
{
    return (cf_limits_t){w, h, colour ? FILE_MAX_COLOUR : FILE_MAX_MONO,
                         colour ? (1u << PROTO_ASSET_FORMAT_MONO1 | 1u << PROTO_ASSET_FORMAT_RGB565 |
                                   1u << PROTO_ASSET_FORMAT_RGB565A8)
                                : 1u << PROTO_ASSET_FORMAT_MONO1,
                         colour};
}

/* The XFER_FINISH check of a face file that is stored as `blob_name`: the whole file is parsed under the board's rules.
 * Needs the file in memory once (PSRAM on the boards) and no UI state: it runs on the transfer's task. */
static UI_EXT_BSS cf_face_t check_face;

proto_status_t ui_face_check(const blob_storage_t *b, const char *blob_name, uint32_t size, const char *id, size_t id_len,
                             uint16_t width, uint16_t height, bool colour)
{
    const cf_limits_t lim = limits_for(width, height, colour);
    char want[CF_NAME_MAX + 1];
    if (id_len > CF_NAME_MAX || size == 0) {
        return PROTO_IMAGE_INVALID;
    }
    memcpy(want, id, id_len);
    want[id_len] = '\0';
    if (size > lim.max_file) {
        return PROTO_TOO_LARGE;
    }
    uint8_t *buf = big_alloc(size);
    if (!buf) {
        return PROTO_NO_SPACE;
    }
    proto_status_t st = PROTO_INTERNAL;
    if (b->read(b->ctx, blob_name, 0, buf, size) == BLOB_OK) {
        st = cf_parse(buf, size, &lim, want, PROTO_CONTRACT_VERSION_PACKED, &check_face, NULL);
    }
    free(buf);
    return st;
}

static bool load(const char *id)
{
    const bool colour = colour_layout();
    const cf_limits_t lim = limits_for(ui.info.width, ui.info.height, colour);
    const ui_builtin_face_t *b = find_builtin(id);
    const uint8_t *data;
    uint8_t *owned = NULL;
    size_t len;
    if (b) {
        data = b->data;
        len = b->len;
    } else {
        owned = read_installed(id, lim.max_file, &len);
        data = owned;
        if (!owned) {
            return false;
        }
    }
    const bool ok = cf_parse(data, len, &lim, id, PROTO_CONTRACT_VERSION_PACKED, &face, NULL) == PROTO_OK &&
                    prepare_images();
    free(owned);
    if (!ok) {
        free_images();
        return false;
    }
    fc.builtin = b != NULL;
    fc.update_drawn = cf_draws_source(&face, PROTO_FACE_SOURCE_UPDATE_AVAILABLE);
    return build_objects();
}

/* ---------------------------------------------------------------- the settings pages' view of the faces */

#define NAME_HEAD 512u /* bytes of a face file that hold its names */

static void entry_names(ui_face_entry_t *e, const uint8_t *head, size_t len)
{
    char names[2][CF_NAME_MAX + 1];
    cf_peek_names(head, len, names);
    snprintf(e->name[0], sizeof(e->name[0]), "%s", names[0]);
    snprintf(e->name[1], sizeof(e->name[1]), "%s", names[1]);
}

size_t ui_faces_collect(ui_face_entry_t *out, size_t max)
{
    size_t n = 0;
    memset(out, 0, max * sizeof(*out));
    if (max) {
        out[n++].builtin = true; /* Classic: id "", its name is a string of the UI */
    }
    const ui_builtin_face_t *list = NULL;
    size_t count = 0;
#if UI_HAS_COLOUR
    if (colour_layout()) {
        list = ui_builtin_faces_colour;
        count = ui_builtin_faces_colour_count;
    }
#endif
#if UI_HAS_MONO
    if (!colour_layout()) {
        list = ui_builtin_faces_mono;
        count = ui_builtin_faces_mono_count;
    }
#endif
    for (size_t i = 0; i < count && n < max; i++, n++) {
        snprintf(out[n].id, sizeof(out[n].id), "%s", list[i].id);
        out[n].builtin = true;
        entry_names(&out[n], list[i].data, list[i].len);
    }
    const blob_storage_t *b = ui.p.blob;
    char name[BLOB_NAME_MAX + 1], meta[BLOB_NAME_MAX + 1];
    uint32_t size, k = 0;
    while (b && n < max && b->list(b->ctx, k++, name, sizeof(name), &size) == BLOB_OK) {
        const size_t len = strlen(name);
        if (strncmp(name, "6_", 2) != 0 || len < 3 || name[len - 1] == '~' || name[len - 1] == '@' || len - 2 > CF_NAME_MAX) {
            continue; /* not a face (the type is the number before the underscore) */
        }
        memcpy(out[n].id, name + 2, len - 2); /* <= CF_NAME_MAX, checked above; the entry is zeroed */
        uint8_t head[NAME_HEAD];
        const size_t h = size < NAME_HEAD ? size : NAME_HEAD;
        if (b->read(b->ctx, name, 0, head, h) == BLOB_OK) {
            entry_names(&out[n], head, h);
        }
        uint8_t m[XFER_VERSION_MAX + 2];
        uint32_t msize = 0;
        memcpy(meta, name, len); /* len <= CF_NAME_MAX + 2: room for the suffix */
        meta[len] = '@';
        meta[len + 1] = '\0';
        if (b->stat(b->ctx, meta, &msize) == BLOB_OK && msize >= 2 && msize <= sizeof(m) &&
            b->read(b->ctx, meta, 0, m, msize) == BLOB_OK && m[msize - 2] == 0) { /* version NUL origin */
            out[n].local = m[msize - 1] == PROTO_ASSET_ORIGIN_LOCAL;
        }
        n++;
    }
    return n;
}

/* The settings pages ask while another screen covers the clock: the face is (re)loaded now if the selection changed. */
void ui_face_list(void (*each)(void *ctx, const char *id, const char *name, bool builtin, bool local, bool active),
                  void *ctx)
{
    static ui_face_entry_t list[UI_FACES_MAX]; /* too big for a task's stack */
    const size_t n = ui_faces_collect(list, UI_FACES_MAX);
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        found |= strcmp(list[i].id, ui.cfg.clock_face_id) == 0;
    }
    for (size_t i = 0; i < n; i++) {
        const bool active = found ? strcmp(list[i].id, ui.cfg.clock_face_id) == 0 : i == 0; /* not installed: Classic */
        each(ctx, list[i].id, list[i].name[0], list[i].builtin, list[i].local, active);
    }
}

size_t ui_face_option_count(void)
{
    ui_face_sync();
    return fc.active ? face.n_options : 0;
}

bool ui_face_option_row(size_t i, const char **label, char *value, size_t cap, uint8_t *id, uint8_t *next)
{
    ui_face_sync();
    if (!fc.active || i >= face.n_options) {
        return false;
    }
    const cf_option_t *o = &face.option[i];
    const bool vi = ui.cfg.language == PROTO_LANGUAGE_VI;
    const uint8_t range = cf_option_range(o), v = fc.values[i];
    *label = vi && o->label[1][0] ? o->label[1] : o->label[0];
    if (o->type == PROTO_FACE_OPTION_TYPE_TOGGLE) {
        snprintf(value, cap, "%s", v ? ui_strings()->on : ui_strings()->off_value);
    } else if (v < o->n_choices && (o->choice[v].label[vi][0] || o->choice[v].label[0][0])) {
        snprintf(value, cap, "%s", vi && o->choice[v].label[1][0] ? o->choice[v].label[1] : o->choice[v].label[0]);
    } else {
        snprintf(value, cap, "%u", (unsigned)v + 1u);
    }
    *id = o->id;
    *next = range ? (uint8_t)((v + 1u) % range) : 0;
    return true;
}

void ui_face_sync(void)
{
    const char *id = ui.cfg.clock_face_id;
    const bool same = fc.loaded && strcmp(fc.id, id) == 0 && fc.gen == ui.assets_gen && fc.w == ui.info.width &&
                      fc.h == ui.info.height;
    if (!same) {
        free_face();
        fc.loaded = true;
        snprintf(fc.id, sizeof(fc.id), "%s", id);
        fc.gen = ui.assets_gen;
        fc.w = ui.info.width;
        fc.h = ui.info.height;
        if (id[0]) {
            fc.active = load(id);
            if (!fc.active) {
                free_face();
                fc.errors++;
            }
        }
        fc.opts_len = 0xFF; /* resolve the options below */
    }
    if (fc.active && (fc.opts_len != ui.cfg.clock_face_opts_len || memcmp(fc.opts, ui.cfg.clock_face_opts, sizeof(fc.opts)) != 0)) {
        fc.opts_len = ui.cfg.clock_face_opts_len > sizeof(fc.opts) ? (uint8_t)sizeof(fc.opts) : ui.cfg.clock_face_opts_len;
        memcpy(fc.opts, ui.cfg.clock_face_opts, sizeof(fc.opts));
        cf_opts_resolve(&face, fc.opts, fc.opts_len, fc.values);
    }
}

bool ui_face_active(void) { return fc.active; }

const char *ui_clock_face_shown(void) { return fc.active ? fc.id : ""; }

uint32_t ui_clock_face_errors(void) { return fc.errors; }

void ui_face_reset(void)
{
    free_face();
    for (unsigned i = 0; i < UI_WEATHER_ICON_COUNT; i++) {
        if (fc.wx_icon[i]) {
            lv_draw_buf_destroy(fc.wx_icon[i]);
        }
    }
    for (unsigned i = 0; i < UI_STATUS_ICON_COUNT; i++) {
        if (fc.status_icon[i]) {
            lv_draw_buf_destroy(fc.status_icon[i]);
        }
    }
    memset(&fc, 0, sizeof(fc));
}

/* ---------------------------------------------------------------- drawing */

static void fill_data(cf_data_t *d)
{
    static char date[48];
    memset(d, 0, sizeof(*d));
    board_datetime_t t;
    d->time_known = ui_local_time(&t);
    if (d->time_known) {
        d->hour = t.hour;
        d->minute = t.min;
        d->day = t.day;
        d->month = t.month;
        d->year = (uint16_t)t.year;
        d->wday = t.wday;
        ui_format_date(date, sizeof(date));
        d->date = date;
    } else {
        d->date = ui_strings()->no_time;
    }
    d->clock_24h = ui.cfg.clock_24h;
    d->language = ui.cfg.language;
    d->fahrenheit = ui.cfg.temp_unit == PROTO_TEMP_UNIT_FAHRENHEIT;
    if (ui.wx_ok) {
        const wx_reading_t *r = &ui.wx;
        d->has_wx_temp = true;
        d->wx_temp_c10 = r->temp_c10;
        d->has_wx_high = r->high_c10 != WX_TEMP_UNKNOWN;
        d->wx_high_c10 = r->high_c10;
        d->has_wx_low = r->low_c10 != WX_TEMP_UNKNOWN;
        d->wx_low_c10 = r->low_c10;
        d->has_wx_rain = true;
        d->wx_rain = r->precip_pct;
        d->has_wx_cond = true;
        d->wx_cond = r->cond;
        d->wx_day = r->is_day;
        d->wx_place = r->place;
    }
    if (ui.p.climate && ui.climate_ok) {
        d->has_room = true;
        d->room_temp_c100 = ui.clim.temp_centi_c;
        d->room_rh_c100 = ui.clim.rh_centi_pct;
    }
    d->battery_pct = ui.st.battery_pct;
    d->charging = ui.st.external_power;
    d->ble = ui.st.connected;
    d->wifi = ui_wifi_icon_for(ui.st.wifi_state) == UI_WIFI_OK;
    d->notif_count = (uint16_t)ui.n_cards;
    d->update_available = ui.upd.mark;
}

static void set_colour(el_t *x, const cf_element_t *e, uint16_t c565)
{
    if (x->colour_valid && x->colour == c565) {
        return;
    }
    x->colour = c565;
    x->colour_valid = true;
    const lv_color_t c = colour_of(c565);
    switch (e->kind) {
    case PROTO_FACE_ELEMENT_RECT:
        lv_obj_set_style_bg_color(x->obj, c, 0);
        lv_obj_set_style_border_color(x->obj, c, 0);
        break;
    case PROTO_FACE_ELEMENT_LINE:
        lv_obj_set_style_bg_color(x->obj, c, 0);
        lv_obj_set_style_line_color(x->obj, c, 0);
        break;
    case PROTO_FACE_ELEMENT_ARC:
        lv_obj_set_style_arc_color(x->obj, c, LV_PART_MAIN);
        lv_obj_set_style_arc_color(x->obj, c, LV_PART_INDICATOR);
        break;
    case PROTO_FACE_ELEMENT_TEXT: lv_obj_set_style_text_color(x->obj, c, 0); break;
    default: break;
    }
}

static void recolor_image(lv_obj_t *o, bool mask, uint16_t c565)
{
    lv_obj_set_style_image_recolor(o, colour_of(c565), 0);
    lv_obj_set_style_image_recolor_opa(o, mask ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void draw_number(const cf_element_t *e, el_t *x, const cf_out_t *o)
{
    const uint8_t *slots = face.set_image[e->set];
    int32_t total = 0;
    unsigned shown = 0;
    const img_t *im[CF_GLYPH_MAX];
    for (unsigned g = 0; g < o->n_glyph && shown < x->n_glyph; g++) {
        const uint8_t idx = o->glyph[g] < CF_SLOTS ? slots[o->glyph[g]] : 0;
        if (idx) {
            im[shown++] = &images[idx - 1];
            total += images[idx - 1].w;
        }
    }
    if (shown) {
        total += (int32_t)e->spacing * (int32_t)(shown - 1);
    }
    int32_t gx = e->x;
    if (e->w > 0) {
        gx += e->align == PROTO_FACE_ALIGN_CENTER ? ((int32_t)e->w - total) / 2
              : e->align == PROTO_FACE_ALIGN_RIGHT ? (int32_t)e->w - total
                                                    : 0;
    }
    for (unsigned g = 0; g < x->n_glyph; g++) {
        lv_obj_t *go = x->glyph[g];
        if (g >= shown) {
            ui_set_hidden(go, true);
            continue;
        }
        lv_image_set_src(go, im[g]->src);
        recolor_image(go, im[g]->mask, o->colour);
        lv_obj_set_pos(go, gx, e->y);
        ui_set_hidden(go, false);
        gx += im[g]->w + e->spacing;
    }
}

/* The icon's picture: a key to compare with what is shown, and the picture, size and whether it is a mask. */
static bool icon_picture(const cf_element_t *e, const cf_out_t *o, int *key, const void **src, int *w, int *h, bool *mask)
{
    *mask = true;
    if (e->icon == PROTO_FACE_ICON_WEATHER) {
        const int idx = ui_weather_icon_for((uint8_t)o->value, o->aux != 0);
        if (idx < 0) {
            return false;
        }
        *key = 1 + idx;
        *w = *h = ICON_WX;
        if (colour_layout()) {
#if UI_HAS_COLOUR
            *src = &ui_weather_icons[idx];
            *mask = false;
            return true;
#else
            return false;
#endif
        }
#if UI_HAS_MONO
        if (!fc.wx_icon[idx]) {
            fc.wx_icon[idx] = mask_from_mono1(ui_mono_wx_icons[idx], UI_MONO_WX_SIZE, UI_MONO_WX_SIZE);
        }
        *src = fc.wx_icon[idx];
        return *src != NULL;
#else
        return false;
#endif
    }
    ui_status_icon_t si;
    switch (e->icon) {
    case PROTO_FACE_ICON_BLE: si = o->value ? UI_STATUS_ICON_BLE_CONNECTED : UI_STATUS_ICON_BLE_DISCONNECTED; break;
    case PROTO_FACE_ICON_WIFI: si = UI_STATUS_ICON_WIFI; break;
    case PROTO_FACE_ICON_CHARGING: si = UI_STATUS_ICON_CHARGING; break;
    default: return false;
    }
    if (!fc.status_icon[si]) {
        fc.status_icon[si] = mask_from_mono1(ui_status_icons[si], UI_STATUS_ICON_SIZE, UI_STATUS_ICON_SIZE);
    }
    *key = 100 + (int)si;
    *src = fc.status_icon[si];
    *w = *h = ICON_STATUS;
    return *src != NULL;
}

static void draw_icon(const cf_element_t *e, el_t *x, const cf_out_t *o)
{
    int key, w, h;
    const void *src;
    bool mask;
    if (!icon_picture(e, o, &key, &src, &w, &h, &mask)) {
        ui_set_hidden(x->obj, true);
        return;
    }
    if (x->shown != key) {
        x->shown = key;
        lv_image_set_src(x->obj, src);
    }
    recolor_image(x->obj, mask, o->colour);
    lv_obj_set_pos(x->obj, e->x + ((int32_t)e->w - w) / 2, e->y + ((int32_t)e->h - h) / 2);
    ui_set_hidden(x->obj, false);
}

static void draw_element(const cf_element_t *e, el_t *x, const cf_out_t *o)
{
    if (e->kind != PROTO_FACE_ELEMENT_NUMBER && !x->obj) {
        return;
    }
    if (!o->visible) {
        if (e->kind == PROTO_FACE_ELEMENT_NUMBER) {
            x->number[0] = '\0';
            for (unsigned g = 0; g < x->n_glyph; g++) {
                ui_set_hidden(x->glyph[g], true);
            }
        } else {
            ui_set_hidden(x->obj, true);
        }
        return;
    }
    switch (e->kind) {
    case PROTO_FACE_ELEMENT_NUMBER:
        {
            size_t k = strlen(o->text);
            k = k < sizeof(x->number) ? k : sizeof(x->number) - 1;
            memcpy(x->number, o->text, k);
            x->number[k] = '\0';
        }
        draw_number(e, x, o);
        return;
    case PROTO_FACE_ELEMENT_ICON: draw_icon(e, x, o); return;
    case PROTO_FACE_ELEMENT_TEXT: ui_set_text(x->obj, o->text); break;
    case PROTO_FACE_ELEMENT_ARC:
        if (x->shown_value != o->value) {
            x->shown_value = (int)o->value;
            lv_arc_set_value(x->obj, (int32_t)o->value);
        }
        break;
    case PROTO_FACE_ELEMENT_IMAGE: recolor_image(x->obj, images[e->image].mask, o->colour); ui_set_hidden(x->obj, false); return;
    default: break;
    }
    set_colour(x, e, o->colour);
    ui_set_hidden(x->obj, false);
}

void ui_face_apply(bool show)
{
    if (!fc.root) {
        return;
    }
    ui_set_hidden(fc.root, !show || !fc.active);
    if (!show || !fc.active) {
        return;
    }
    cf_data_t d;
    fill_data(&d);
    for (unsigned i = 0; i < face.n_elements; i++) {
        cf_out_t o;
        cf_eval(&face, fc.values, &d, i, &o);
        draw_element(&face.element[i], &els[i], &o);
    }
    /* the firmware's own parts: the notification badge on the built-in faces, the update mark when the face does
     * not draw update_available itself */
    const bool badge = fc.builtin && ui.n_cards > 0 && ui.st.ota_pct < 0;
    ui_set_hidden(fc.badge, !badge);
    if (badge) {
        char num[8];
        snprintf(num, sizeof(num), "%u", (unsigned)ui.n_cards);
        ui_set_text(fc.badge, num);
    }
    ui_set_hidden(fc.mark, !(ui.upd.mark && !fc.update_drawn));
}

/* The text the face shows now, one line per element, for ui_visible_text (tests and tools). */
size_t ui_face_text(char *out, size_t cap)
{
    size_t n = 0;
    if (!cap) {
        return 0;
    }
    out[0] = '\0';
    if (!fc.active || !fc.root || lv_obj_has_flag(fc.root, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    const unsigned n_el = face.n_elements;
    for (unsigned i = 0; i < n_el + 2; i++) {
        const char *t = "";
        if (i == n_el) { /* the firmware's parts: the badge, then the update mark */
            if (fc.badge && !lv_obj_has_flag(fc.badge, LV_OBJ_FLAG_HIDDEN)) {
                t = lv_label_get_text(fc.badge);
            }
        } else if (i == n_el + 1) {
            if (fc.mark && !lv_obj_has_flag(fc.mark, LV_OBJ_FLAG_HIDDEN)) {
                t = lv_label_get_text(fc.mark);
            }
        } else if (face.element[i].kind == PROTO_FACE_ELEMENT_NUMBER) {
            t = els[i].number;
        } else if (face.element[i].kind == PROTO_FACE_ELEMENT_TEXT && els[i].obj &&
                   !lv_obj_has_flag(els[i].obj, LV_OBJ_FLAG_HIDDEN)) {
            t = lv_label_get_text(els[i].obj);
        }
        if (!t[0]) {
            continue;
        }
        const int k = snprintf(out + n, cap - n, "%s\n", t);
        if (k < 0 || (size_t)k >= cap - n) {
            return cap - 1;
        }
        n += (size_t)k;
    }
    return n;
}
