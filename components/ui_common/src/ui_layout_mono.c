/*
 * The Paper screens (mono layout, PLAN D51, D78), drawn on a 1-bit display through the frame backend: a
 * PLACEHOLDER notification card screen that exercises the pipeline -> UI -> display path until the real
 * Paper UI is written; the idle screen without notifications: one rounded box with a large clock and the
 * date, the weather (icon, high / low, chance of rain) and, with the sensor, the temperature and humidity
 * next to their icons; the full-screen pairing screen; and the Wi-Fi setup screen with its QR code.
 */
#include <stdio.h>
#include <string.h>

#include "epager/blob_storage.h"
#include "epager/board_util.h"
#include "epager/proto_gen.h"
#include "epager/xfer.h"
#include "src/libs/qrcode/qrcodegen.h"
#include "ui_icons.h"
#include "ui_priv.h"

LV_FONT_DECLARE(ui_font_regular_16)
LV_FONT_DECLARE(ui_font_bold_16)
LV_FONT_DECLARE(ui_font_clock_56) /* digits, colon, minus */
LV_FONT_DECLARE(ui_font_bold_24)  /* digits and the climate values' symbols */

#define BAR_H 24
#define GAP 2
#define MARGIN 4
#define ICON_GAP 4 /* between the icon and the title */
#define ICON_TEXT_X (MARGIN + (int32_t)UI_ICON_SIZE + ICON_GAP)
/* idle screen: no header, one rounded box; the labels are placed from the font metrics so the digits' ink is
 * centred, and the rows share the box's height evenly */
#define BOX_BORDER 2
#define BOX_RADIUS 8
#define BOX_GAP 4
#define DATE_GAP 10      /* clock digits to the date */
#define AMPM_GAP 4
#define WX_ICON_GAP 6    /* weather icon to the high / low */
#define WX_TEXT_GAP 12   /* high / low to the umbrella */
#define WX_ICON_GAP_TIGHT 4
#define WX_TEXT_GAP_TIGHT 6
#define ENV_ICON_GAP 3   /* thermometer or drop to its value */
#define ENV_GROUP_GAP 10 /* temperature to humidity */
#define RULE_INSET 12    /* the line above the sensor row, from the box's inner edge */
#define BADGE_W 30       /* clock screen: how many notifications wait */
#define BADGE_H 22
#define LINK_GAP 4       /* status bar: connection icon to the battery text, and to the left text */
#define PAIR_TITLE_Y 10
#define PAIR_TEXT_Y 34
#define QR_PX 100        /* the QR code's canvas; whole pixels per module inside it (3 for up to 33 modules) */
#define QR_Y 4
#define SET_ROW_H 25     /* settings list (D96, D97): seven rows under the header fill the panel */
#define SET_Y (BAR_H + 1)
#define QR_VERSION_MAX 10
#define QR_BUF qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VERSION_MAX)

typedef struct {
    lv_obj_t *obj;
    lv_draw_buf_t *buf;
    int shown;                       /* what the canvas holds (an index), -1 none yet */
} icon_t;

static struct {
    lv_obj_t *bar, *app, *counter, *icon, *title, *body, *rule, *foot_left, *foot_right, *off;
    lv_obj_t *clock_box, *clock, *ampm, *date, *badge, *divider;
    lv_obj_t *wx_hilo, *wx_rain, *temp, *rh;
    icon_t wx_icon, rain_icon, temp_icon, rh_icon, wifi_icon, bolt_icon; /* the status line: the fan left of the link icon, the bolt left of the level */
    lv_obj_t *pair_title, *pair_text, *pair_code, *pair_keys, *pair_box;
    lv_obj_t *qr, *setup_text, *setup_key;
    lv_obj_t *notice_bar, *notice; /* a short message over the footer */
    lv_obj_t *set_bg[UI_SETTINGS_ROWS], *set_name[UI_SETTINGS_ROWS], *set_value[UI_SETTINGS_ROWS]; /* settings list */
    char qr_data[160];               /* what the QR code holds now */
    uint8_t qr_tmp[QR_BUF], qr_code[QR_BUF];
    int32_t box_bottom;
    int32_t clock_ink_y;             /* top of the clock digits' ink */
    lv_draw_buf_t *icon_buf;
    lv_obj_t *link;                  /* status bar: Bluetooth connected / not connected */
    lv_draw_buf_t *link_buf;
    int32_t body_bottom;
} m;

static void load_mono1(lv_obj_t *canvas, lv_draw_buf_t *buf, const uint8_t *src, uint32_t size);

/* The QR code's pixels: A8, which LVGL blends directly. Static, not in LVGL's heap: an A1 image of this size is
 * converted to A8 in the pool for every draw, and the pool is too small for it (the colour layout does the same). */
static UI_EXT_BSS LV_ATTRIBUTE_MEM_ALIGN uint8_t qr_px[LV_DRAW_BUF_SIZE(QR_PX, QR_PX, LV_COLOR_FORMAT_A8)];
static lv_draw_buf_t qr_buf;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, int32_t x, int32_t y, int32_t w, int32_t h,
                       lv_label_long_mode_t mode, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_label_set_long_mode(l, mode);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text_static(l, "");
    return l;
}

/* Distance from a label's top to the top of the digits (glyph '0') of its font. */
static int32_t ink_top(const lv_font_t *f)
{
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(f, &g, '0', 0)) {
        return 0;
    }
    return f->line_height - f->base_line - (g.ofs_y + (int32_t)g.box_h);
}

/* Height of the digits' ink of a font (glyph '0'). */
static int32_t ink_h(const lv_font_t *f)
{
    lv_font_glyph_dsc_t g;
    return lv_font_get_glyph_dsc(f, &g, '0', 0) ? (int32_t)g.box_h : f->line_height;
}

static lv_obj_t *box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_black(), 0);
    lv_obj_set_style_border_width(o, BOX_BORDER, 0);
    lv_obj_set_style_radius(o, BOX_RADIUS, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* An A1 canvas for a mono icon: bit 1 = ink, drawn black. */
static void icon_init(icon_t *i, lv_obj_t *scr, uint32_t size)
{
    i->shown = -1;
    i->buf = lv_draw_buf_create(size, size, LV_COLOR_FORMAT_A1, 0);
    if (!i->buf) {
        return;
    }
    i->obj = lv_canvas_create(scr);
    lv_canvas_set_draw_buf(i->obj, i->buf);
    lv_obj_set_style_image_recolor(i->obj, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(i->obj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(i->obj, LV_OBJ_FLAG_CLICKABLE);
}

/* Show icon number `index` (the data `src`) on the canvas, only when it changed. */
static void icon_show(icon_t *i, int index, const uint8_t *src, uint32_t size)
{
    if (!i->obj) {
        return;
    }
    if (i->shown != index) {
        i->shown = index;
        load_mono1(i->obj, i->buf, src, size);
    }
}

static lv_obj_t *hline(lv_obj_t *scr)
{
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_set_size(o, 1, 1);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void build_idle(lv_obj_t *scr, int32_t w, int32_t foot_y)
{
    const lv_color_t black = lv_color_black();
    m.box_bottom = foot_y - BOX_GAP; /* the footer's status line stays */
    m.clock_box = box(scr, MARGIN, MARGIN, w - 2 * MARGIN, m.box_bottom - MARGIN);
    m.clock = label(scr, &ui_font_clock_56, 0, 0, LV_SIZE_CONTENT, ui_font_clock_56.line_height,
                     LV_LABEL_LONG_MODE_CLIP, black);
    m.ampm = label(scr, &ui_font_bold_16, 0, 0, LV_SIZE_CONTENT, ui_font_bold_16.line_height,
                    LV_LABEL_LONG_MODE_CLIP, black);
    m.date = label(scr, &ui_font_bold_16, MARGIN + BOX_BORDER, 0, w - 2 * (MARGIN + BOX_BORDER),
                    ui_font_bold_16.line_height, LV_LABEL_LONG_MODE_DOTS, black);
    lv_obj_set_style_text_align(m.date, LV_TEXT_ALIGN_CENTER, 0);
    icon_init(&m.wx_icon, scr, UI_MONO_WX_SIZE);
    m.wx_hilo = label(scr, &ui_font_bold_16, 0, 0, LV_SIZE_CONTENT, ui_font_bold_16.line_height,
                      LV_LABEL_LONG_MODE_CLIP, black);
    icon_init(&m.rain_icon, scr, UI_MONO_RAIN_SIZE);
    m.wx_rain = label(scr, &ui_font_bold_16, 0, 0, LV_SIZE_CONTENT, ui_font_bold_16.line_height,
                      LV_LABEL_LONG_MODE_CLIP, black);
    m.divider = hline(scr);
    icon_init(&m.temp_icon, scr, UI_MONO_ENV_SIZE);
    m.temp = label(scr, &ui_font_bold_24, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT, LV_LABEL_LONG_MODE_CLIP, black);
    icon_init(&m.rh_icon, scr, UI_MONO_ENV_SIZE);
    m.rh = label(scr, &ui_font_bold_24, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT, LV_LABEL_LONG_MODE_CLIP, black);
    /* the badge: white digits on a black pill in the footer's left part, empty on the idle screen */
    m.badge = label(scr, &ui_font_bold_16, MARGIN + BOX_BORDER, foot_y + (BAR_H - BADGE_H) / 2, BADGE_W, BADGE_H,
                    LV_LABEL_LONG_MODE_CLIP, lv_color_white());
    lv_obj_set_style_text_align(m.badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(m.badge, black, 0);
    lv_obj_set_style_bg_opa(m.badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(m.badge, BADGE_H / 2, 0);
    lv_obj_set_style_pad_top(m.badge, (BADGE_H - (int32_t)ui_font_bold_16.line_height) / 2, 0);
}

/* The full-screen pairing screen (the code a phone asks for takes the text's place) and the Wi-Fi setup screen. */
static void build_screens(lv_obj_t *scr, int32_t w, int32_t foot_y)
{
    const lv_color_t black = lv_color_black();
    const int32_t inner = w - 2 * MARGIN;
    m.pair_title = label(scr, &ui_font_bold_16, MARGIN, PAIR_TITLE_Y, inner, ui_font_bold_16.line_height,
                         LV_LABEL_LONG_MODE_CLIP, black);
    lv_obj_set_style_text_align(m.pair_title, LV_TEXT_ALIGN_CENTER, 0);
    m.pair_text = label(scr, &ui_font_regular_16, MARGIN, PAIR_TEXT_Y, inner, 5 * ui_font_regular_16.line_height,
                        LV_LABEL_LONG_MODE_WRAP, black);
    lv_obj_set_style_text_align(m.pair_text, LV_TEXT_ALIGN_CENTER, 0);
    m.pair_box = box(scr, (w - 130) / 2, 86, 130, 46); /* the passkey */
    m.pair_code = label(scr, &ui_font_bold_24, 0, 0, LV_SIZE_CONTENT, ui_font_bold_24.line_height,
                        LV_LABEL_LONG_MODE_CLIP, black);
    m.pair_keys = label(scr, &ui_font_regular_16, MARGIN, foot_y - 2 * ui_font_regular_16.line_height - 6, inner,
                        2 * ui_font_regular_16.line_height, LV_LABEL_LONG_MODE_CLIP, black);
    lv_obj_set_style_text_align(m.pair_keys, LV_TEXT_ALIGN_CENTER, 0);

    lv_draw_buf_init(&qr_buf, QR_PX, QR_PX, LV_COLOR_FORMAT_A8, 0, qr_px, sizeof(qr_px));
    lv_draw_buf_set_flag(&qr_buf, LV_IMAGE_FLAGS_MODIFIABLE);
    lv_draw_buf_clear(&qr_buf, NULL);
    m.qr = lv_canvas_create(scr);
    lv_canvas_set_draw_buf(m.qr, &qr_buf);
    lv_obj_set_pos(m.qr, (w - QR_PX) / 2, QR_Y);
    lv_obj_set_style_image_recolor(m.qr, black, 0);
    lv_obj_set_style_image_recolor_opa(m.qr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(m.qr, LV_OBJ_FLAG_CLICKABLE);
    m.setup_text = label(scr, &ui_font_regular_16, MARGIN, 0, inner, 0, LV_LABEL_LONG_MODE_WRAP, black);
    lv_obj_set_style_text_align(m.setup_text, LV_TEXT_ALIGN_CENTER, 0);
    m.setup_key = label(scr, &ui_font_regular_16, MARGIN, foot_y + (BAR_H - ui_font_regular_16.line_height) / 2, inner,
                        ui_font_regular_16.line_height, LV_LABEL_LONG_MODE_CLIP, black);
    lv_obj_set_style_text_align(m.setup_key, LV_TEXT_ALIGN_CENTER, 0);
}

/* The settings list: rows under the header (the highlighted one is a black bar with white text). */
static void build_settings(lv_obj_t *scr, int32_t w)
{
    const lv_color_t black = lv_color_black();
    const int32_t inner = w - 2 * (MARGIN + 2);
    for (size_t i = 0; i < UI_SETTINGS_ROWS; i++) {
        const int32_t y = SET_Y + (int32_t)i * SET_ROW_H, text_y = y + (SET_ROW_H - ui_font_regular_16.line_height) / 2;
        m.set_bg[i] = lv_obj_create(scr);
        lv_obj_set_pos(m.set_bg[i], 2, y);
        lv_obj_set_size(m.set_bg[i], w - 4, SET_ROW_H);
        lv_obj_set_style_bg_color(m.set_bg[i], black, 0);
        lv_obj_set_style_bg_opa(m.set_bg[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(m.set_bg[i], 0, 0);
        lv_obj_set_style_radius(m.set_bg[i], 4, 0);
        lv_obj_set_style_pad_all(m.set_bg[i], 0, 0);
        lv_obj_remove_flag(m.set_bg[i], LV_OBJ_FLAG_SCROLLABLE);
        m.set_name[i] = label(scr, &ui_font_regular_16, MARGIN + 2, text_y, inner, ui_font_regular_16.line_height,
                              LV_LABEL_LONG_MODE_DOTS, black);
        m.set_value[i] = label(scr, &ui_font_bold_16, MARGIN + 2, text_y, inner, ui_font_bold_16.line_height,
                               LV_LABEL_LONG_MODE_CLIP, black);
        lv_obj_set_style_text_align(m.set_value[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static void build(void)
{
    const int32_t w = ui.info.width, h = ui.info.height;
    const lv_color_t black = lv_color_black(), white = lv_color_white();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, white, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    m.bar = lv_obj_create(scr);
    lv_obj_set_pos(m.bar, 0, 0);
    lv_obj_set_size(m.bar, w, BAR_H);
    lv_obj_set_style_bg_color(m.bar, black, 0);
    lv_obj_set_style_bg_opa(m.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m.bar, 0, 0);
    lv_obj_set_style_radius(m.bar, 0, 0);
    lv_obj_set_style_pad_all(m.bar, 0, 0);
    lv_obj_remove_flag(m.bar, LV_OBJ_FLAG_SCROLLABLE);
    m.app = label(m.bar, &ui_font_regular_16, MARGIN, 0, w - 2 * MARGIN - 44, BAR_H, LV_LABEL_LONG_MODE_DOTS, white);
    m.counter = label(m.bar, &ui_font_regular_16, w - MARGIN - 40, 0, 40, BAR_H, LV_LABEL_LONG_MODE_CLIP, white);
    lv_obj_set_style_text_align(m.counter, LV_TEXT_ALIGN_RIGHT, 0);

    const int32_t foot_y = h - BAR_H;
    /* A1 canvas: alpha-only, bit 1 = ink, drawn in the recolor colour (black on the card). mono1 has
     * bit 1 = white, so the icon is inverted when copied in. (LVGL 9.5's indexed I1 image path draws
     * nothing onto an I1 display, alpha-only images take the mask path.) */
    m.icon_buf = lv_draw_buf_create(UI_ICON_SIZE, UI_ICON_SIZE, LV_COLOR_FORMAT_A1, 0);
    if (m.icon_buf) {
        m.icon = lv_canvas_create(scr);
        lv_canvas_set_draw_buf(m.icon, m.icon_buf);
        lv_obj_set_pos(m.icon, MARGIN, BAR_H + GAP);
        lv_obj_set_style_image_recolor(m.icon, black, 0);
        lv_obj_set_style_image_recolor_opa(m.icon, LV_OPA_COVER, 0);
    }
    m.title = label(scr, &ui_font_bold_16, MARGIN, BAR_H + GAP, w - 2 * MARGIN, ui_font_bold_16.line_height,
                     LV_LABEL_LONG_MODE_DOTS, black);
    m.body = label(scr, &ui_font_regular_16, MARGIN, 0, w - 2 * MARGIN, 0, LV_LABEL_LONG_MODE_DOTS, black);
    m.body_bottom = foot_y - GAP;

    m.rule = lv_obj_create(scr);
    lv_obj_set_pos(m.rule, 0, foot_y - 1);
    lv_obj_set_size(m.rule, w, 1);
    lv_obj_set_style_bg_color(m.rule, black, 0);
    lv_obj_set_style_bg_opa(m.rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m.rule, 0, 0);
    lv_obj_set_style_radius(m.rule, 0, 0);
    m.foot_left = label(scr, &ui_font_regular_16, MARGIN, foot_y, w / 2 + 20, BAR_H, LV_LABEL_LONG_MODE_CLIP, black);
    m.foot_right = label(scr, &ui_font_regular_16, w / 2 + 24, foot_y, w / 2 - 24 - MARGIN, BAR_H,
                          LV_LABEL_LONG_MODE_CLIP, black);
    lv_obj_set_style_text_align(m.foot_right, LV_TEXT_ALIGN_RIGHT, 0);
    m.link_buf = lv_draw_buf_create(UI_STATUS_ICON_SIZE, UI_STATUS_ICON_SIZE, LV_COLOR_FORMAT_A1, 0);
    if (m.link_buf) {
        m.link = lv_canvas_create(scr); /* A1 like the card icon */
        lv_canvas_set_draw_buf(m.link, m.link_buf);
        lv_obj_set_style_image_recolor(m.link, black, 0);
        lv_obj_set_style_image_recolor_opa(m.link, LV_OPA_COVER, 0);
    }

    icon_init(&m.wifi_icon, scr, UI_STATUS_ICON_SIZE);
    icon_init(&m.bolt_icon, scr, UI_STATUS_ICON_SIZE);
    build_idle(scr, w, foot_y);
    build_screens(scr, w, foot_y);
    build_settings(scr, w);

    m.notice_bar = lv_obj_create(scr);
    lv_obj_set_pos(m.notice_bar, 0, foot_y);
    lv_obj_set_size(m.notice_bar, w, BAR_H);
    lv_obj_set_style_bg_color(m.notice_bar, black, 0);
    lv_obj_set_style_bg_opa(m.notice_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m.notice_bar, 0, 0);
    lv_obj_set_style_radius(m.notice_bar, 0, 0);
    lv_obj_set_style_pad_all(m.notice_bar, 0, 0);
    lv_obj_remove_flag(m.notice_bar, LV_OBJ_FLAG_SCROLLABLE);
    m.notice = label(scr, &ui_font_bold_16, MARGIN, foot_y + (BAR_H - ui_font_bold_16.line_height) / 2, w - 2 * MARGIN,
                     ui_font_bold_16.line_height, LV_LABEL_LONG_MODE_DOTS, white);
    lv_obj_set_style_text_align(m.notice, LV_TEXT_ALIGN_CENTER, 0);

    m.off = label(scr, &ui_font_bold_16, 0, h / 2 - ui_font_bold_16.line_height, w, 2 * ui_font_bold_16.line_height,
                   LV_LABEL_LONG_MODE_CLIP, black);
    lv_obj_set_style_text_align(m.off, LV_TEXT_ALIGN_CENTER, 0);
}

/* The title is one line (to the right of the icon, when there is one; too long ends in dots); the body
 * starts under the taller of the two and fills whole lines down to the footer. */
static void layout_text(bool with_icon)
{
    const lv_font_t *tf = &ui_font_bold_16, *bf = ui.st.has_passkey ? &ui_font_bold_24 : &ui_font_regular_16;
    const int32_t x = with_icon ? ICON_TEXT_X : MARGIN;
    lv_obj_set_x(m.title, x);
    lv_obj_set_width(m.title, ui.info.width - MARGIN - x);
    int32_t head_h = tf->line_height;
    if (with_icon && head_h < (int32_t)UI_ICON_SIZE) {
        head_h = (int32_t)UI_ICON_SIZE;
    }
    int32_t body_y = BAR_H + GAP + head_h + GAP;
    lv_obj_set_y(m.body, body_y);
    lv_obj_set_height(m.body, (m.body_bottom - body_y) / bf->line_height * bf->line_height);
}

/* An installed app_icon for this app (asset-formats.md): container checked again here, the loader never
 * trusts what it reads. Any mismatch means "no icon", and the category icon is used. */
static bool load_app_icon(const char *app_id, size_t len, uint8_t out[UI_ICON_BYTES])
{
    const blob_storage_t *b = ui.p.blob;
    char id[XFER_APP_ICON_ID_LEN + 1], name[BLOB_NAME_MAX + 1];
    uint8_t file[XFER_ASSET_HEADER_LEN + UI_ICON_BYTES];
    uint32_t size = 0;
    if (!b) {
        return false;
    }
    xfer_app_icon_id(app_id, len, id);
    xfer_asset_blob_name(PROTO_ASSET_TYPE_APP_ICON, id, XFER_APP_ICON_ID_LEN, name);
    if (b->stat(b->ctx, name, &size) != BLOB_OK || size != sizeof(file) ||
        b->read(b->ctx, name, 0, file, sizeof(file)) != BLOB_OK) {
        return false;
    }
    const uint16_t w = (uint16_t)(file[8] | file[9] << 8), h = (uint16_t)(file[10] | file[11] << 8);
    const uint32_t payload = (uint32_t)file[12] | (uint32_t)file[13] << 8 | (uint32_t)file[14] << 16 |
                             (uint32_t)file[15] << 24;
    if (memcmp(file, "ESPA", 4) != 0 || file[4] != 1 || file[5] != PROTO_ASSET_TYPE_APP_ICON ||
        file[6] != PROTO_ASSET_FORMAT_MONO1 || file[7] != 0 || w != UI_ICON_SIZE || h != UI_ICON_SIZE ||
        payload != UI_ICON_BYTES) {
        return false;
    }
    memcpy(out, file + XFER_ASSET_HEADER_LEN, UI_ICON_BYTES);
    return true;
}

/* Copy a mono1 icon (bit 1 = white) into an A1 canvas buffer (bit 1 = ink). */
static void load_mono1(lv_obj_t *canvas, lv_draw_buf_t *buf, const uint8_t *src, uint32_t size)
{
    const uint32_t row_bytes = size / 8u;
    for (uint32_t y = 0; y < size; y++) {
        uint8_t *row = lv_draw_buf_goto_xy(buf, 0, y);
        for (uint32_t i = 0; i < row_bytes; i++) {
            row[i] = (uint8_t)~src[y * row_bytes + i];
        }
    }
    lv_obj_invalidate(canvas);
}

/* Put the notification's icon on the canvas: the app's own, else the built-in one of its category. */
static void show_icon(const npipe_notif_t *n)
{
    uint8_t bits[UI_ICON_BYTES];
    const uint8_t *src = bits;
    if (load_app_icon(n->app_id, n->app_id_len, bits)) {
        ui.icon_src = UI_ICON_APP;
    } else {
        src = ui_builtin_icons[n->category < UI_BUILTIN_ICON_COUNT ? n->category : PROTO_CATEGORY_FALLBACK];
        ui.icon_src = UI_ICON_BUILTIN;
    }
    load_mono1(m.icon, m.icon_buf, src, UI_ICON_SIZE);
}

/* The weather row, centred, its icon's top at `top`: icon, "32°/25°", umbrella and the chance of rain. */
static void apply_weather(int32_t top)
{
    const wx_reading_t *r = &ui.wx;
    const int icon = ui_weather_icon_for(r->cond, r->is_day);
    const bool hilo = r->high_c10 != WX_TEMP_UNKNOWN && r->low_c10 != WX_TEMP_UNKNOWN, rain = r->precip_pct <= 100;
    char hl[48] = "", pct[8] = "";
    if (hilo) {
        snprintf(hl, sizeof(hl), "%ld\u00b0/%ld\u00b0", (long)ui_temp_shown(r->high_c10), (long)ui_temp_shown(r->low_c10));
    }
    if (rain) {
        snprintf(pct, sizeof(pct), "%u%%", r->precip_pct);
    }
    ui_set_hidden(m.wx_icon.obj, icon < 0);
    if (icon >= 0) {
        icon_show(&m.wx_icon, icon, ui_mono_wx_icons[icon], UI_MONO_WX_SIZE);
    }
    ui_set_hidden(m.wx_hilo, !hilo);
    ui_set_hidden(m.rain_icon.obj, !rain);
    ui_set_hidden(m.wx_rain, !rain);
    if (rain) {
        icon_show(&m.rain_icon, 0, ui_mono_rain_icon[0], UI_MONO_RAIN_SIZE);
    }
    ui_set_text(m.wx_hilo, hl);
    ui_set_text(m.wx_rain, pct);
    lv_point_t hs = {0, 0}, ps = {0, 0};
    if (hilo) {
        lv_text_get_size(&hs, hl, &ui_font_bold_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }
    if (rain) {
        lv_text_get_size(&ps, pct, &ui_font_bold_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }
    const int32_t icon_w = icon >= 0 ? (int32_t)UI_MONO_WX_SIZE : 0;
    const int32_t rain_w = rain ? (int32_t)UI_MONO_RAIN_SIZE + 2 + ps.x : 0;
    const int32_t max_w = (int32_t)ui.info.width - 2 * (MARGIN + BOX_BORDER + 4);
    int32_t icon_gap = WX_ICON_GAP, text_gap = WX_TEXT_GAP; /* closer together for long readings (-10°/-15°, 100%) */
    int32_t width;
    for (;;) {
        width = icon_w + (hilo ? (icon_w ? icon_gap : 0) + hs.x : 0);
        width += rain ? (width ? text_gap : 0) + rain_w : 0;
        if (width <= max_w || text_gap <= WX_TEXT_GAP_TIGHT) {
            break;
        }
        icon_gap = WX_ICON_GAP_TIGHT;
        text_gap = WX_TEXT_GAP_TIGHT;
    }
    int32_t x = ((int32_t)ui.info.width - width) / 2;
    const int32_t mid = top + (int32_t)UI_MONO_WX_SIZE / 2;
    if (icon >= 0) {
        lv_obj_set_pos(m.wx_icon.obj, x, top);
        x += icon_w + icon_gap;
    }
    if (hilo) {
        lv_obj_set_pos(m.wx_hilo, x, mid - ui_font_bold_16.line_height / 2);
        x += hs.x + text_gap;
    }
    if (rain) {
        lv_obj_set_pos(m.rain_icon.obj, x, mid - (int32_t)UI_MONO_RAIN_SIZE / 2);
        lv_obj_set_pos(m.wx_rain, x + (int32_t)UI_MONO_RAIN_SIZE + 2, mid - ui_font_bold_16.line_height / 2);
    }
}

/* The sensor row, centred, its icons' top at `top`: thermometer and temperature, drop and humidity. */
static void apply_env(int32_t top)
{
    const char *unit = ui.cfg.temp_unit == PROTO_TEMP_UNIT_FAHRENHEIT ? "\u00b0F" : "\u00b0C";
    char t[16], h[8];
    if (!ui.climate_ok) {
        snprintf(t, sizeof(t), "--.-%s", unit);
        snprintf(h, sizeof(h), "--%%");
    } else {
        int32_t temp, rh;
        ui_climate_shown(&ui.clim, &temp, &rh);
        const int32_t a = temp < 0 ? -temp : temp;
        snprintf(t, sizeof(t), "%s%d.%d%s", temp < 0 ? "-" : "", (int)(a / 10), (int)(a % 10), unit);
        snprintf(h, sizeof(h), "%d%%", (int)rh);
    }
    icon_show(&m.temp_icon, UI_MONO_ENV_THERMOMETER, ui_mono_env_icons[UI_MONO_ENV_THERMOMETER], UI_MONO_ENV_SIZE);
    icon_show(&m.rh_icon, UI_MONO_ENV_HUMIDITY, ui_mono_env_icons[UI_MONO_ENV_HUMIDITY], UI_MONO_ENV_SIZE);
    ui_set_text(m.temp, t);
    ui_set_text(m.rh, h);
    /* the big digits when both values fit beside their icons, else (extreme readings) the small bold font */
    const lv_font_t *f = &ui_font_bold_24;
    lv_point_t ts, hs;
    const int32_t isz = (int32_t)UI_MONO_ENV_SIZE, max_w = (int32_t)ui.info.width - 2 * (MARGIN + BOX_BORDER + 4);
    for (;;) {
        lv_text_get_size(&ts, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_text_get_size(&hs, h, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (f == &ui_font_bold_16 || 2 * isz + 2 * ENV_ICON_GAP + ts.x + hs.x + ENV_GROUP_GAP <= max_w) {
            break;
        }
        f = &ui_font_bold_16;
    }
    lv_obj_set_style_text_font(m.temp, f, 0);
    lv_obj_set_style_text_font(m.rh, f, 0);
    const int32_t width = 2 * isz + 2 * ENV_ICON_GAP + ts.x + hs.x + ENV_GROUP_GAP;
    int32_t x = ((int32_t)ui.info.width - width) / 2;
    const int32_t ty = top + (isz - ink_h(f)) / 2 - ink_top(f);
    lv_obj_set_pos(m.temp_icon.obj, x, top);
    x += isz + ENV_ICON_GAP;
    lv_obj_set_pos(m.temp, x, ty);
    x += ts.x + ENV_GROUP_GAP;
    lv_obj_set_pos(m.rh_icon.obj, x, top);
    x += isz + ENV_ICON_GAP;
    lv_obj_set_pos(m.rh, x, ty);
}

/* The idle screen: the clock block, the weather row (with a reading) and the sensor row (with a sensor) share
 * the box's height with equal gaps. */
static void apply_idle(void)
{
    const ui_strings_t *s = ui_strings();
    char buf[48];
    const char *ampm = NULL;
    if (ui.utc) {
        ui_format_time(buf, sizeof(buf), &ampm, true);
        lv_label_set_text(m.clock, buf);
        ui_format_date(buf, sizeof(buf));
        lv_label_set_text(m.date, buf);
    } else {
        lv_label_set_text(m.clock, "--:--");
        lv_label_set_text(m.date, s->no_time);
    }
    const bool wx = ui.wx_ok, env = ui.p.climate;
    ui_set_hidden(m.wx_icon.obj, !wx);
    ui_set_hidden(m.wx_hilo, !wx);
    ui_set_hidden(m.rain_icon.obj, !wx);
    ui_set_hidden(m.wx_rain, !wx);
    ui_set_hidden(m.divider, !env);
    ui_set_hidden(m.temp_icon.obj, !env);
    ui_set_hidden(m.temp, !env);
    ui_set_hidden(m.rh_icon.obj, !env);
    ui_set_hidden(m.rh, !env);

    /* vertical budget: block, weather row, sensor row, and a gap above, between and below them */
    const int32_t block_h = ink_h(&ui_font_clock_56) + DATE_GAP + ink_h(&ui_font_bold_16);
    const int32_t top = MARGIN + BOX_BORDER, bottom = m.box_bottom - BOX_BORDER;
    const int32_t wx_h = wx ? (int32_t)UI_MONO_WX_SIZE : 0, env_h = env ? (int32_t)UI_MONO_ENV_SIZE : 0;
    const int32_t rows = 1 + (wx ? 1 : 0) + (env ? 1 : 0);
    int32_t gap = (bottom - top - block_h - wx_h - env_h) / (rows + 1);
    if (gap < 2) {
        gap = 2;
    }
    m.clock_ink_y = top + gap;
    const int32_t date_ink_y = m.clock_ink_y + ink_h(&ui_font_clock_56) + DATE_GAP;
    lv_obj_set_y(m.date, date_ink_y - ink_top(&ui_font_bold_16));
    /* centre the clock, with AM/PM to its right on the digits' baseline */
    lv_point_t cs, as = {0, 0};
    lv_text_get_size(&cs, lv_label_get_text(m.clock), &ui_font_clock_56, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    ui_set_hidden(m.ampm, ampm == NULL);
    if (ampm) {
        lv_label_set_text(m.ampm, ampm);
        lv_text_get_size(&as, ampm, &ui_font_bold_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        as.x += AMPM_GAP;
    }
    const int32_t x = (ui.info.width - cs.x - as.x) / 2, y = m.clock_ink_y - ink_top(&ui_font_clock_56);
    lv_obj_set_pos(m.clock, x, y);
    const int32_t baseline = y + ui_font_clock_56.line_height - ui_font_clock_56.base_line;
    lv_obj_set_pos(m.ampm, x + cs.x + AMPM_GAP,
                   baseline - (ui_font_bold_16.line_height - ui_font_bold_16.base_line));

    int32_t row_y = m.clock_ink_y + block_h + gap;
    if (wx) {
        apply_weather(row_y);
        row_y += wx_h + gap;
    }
    if (env) {
        lv_obj_set_pos(m.divider, MARGIN + BOX_BORDER + RULE_INSET, row_y - gap / 2);
        lv_obj_set_size(m.divider, (int32_t)ui.info.width - 2 * (MARGIN + BOX_BORDER + RULE_INSET), 1);
        apply_env(row_y);
    }
}

/* The full-screen pairing screen; with a passkey in progress, the code takes the text's place. */
static void apply_pairing(void)
{
    const ui_strings_t *s = ui_strings();
    const bool code = ui.st.has_passkey;
    lv_label_set_text(m.pair_title, code ? s->pair_code_short : s->pairing);
    lv_label_set_text(m.pair_text, code ? s->pair_code : s->pair_hint);
    ui_set_hidden(m.pair_box, !code);
    ui_set_hidden(m.pair_code, !code);
    ui_set_hidden(m.pair_keys, code);
    if (code) {
        char digits[8];
        snprintf(digits, sizeof(digits), "%06u", (unsigned)(ui.st.passkey % 1000000u));
        lv_label_set_text(m.pair_code, digits);
        lv_point_t ds;
        lv_text_get_size(&ds, digits, &ui_font_bold_24, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_pos(m.pair_code, ((int32_t)ui.info.width - ds.x) / 2,
                       86 + (46 - ink_h(&ui_font_bold_24)) / 2 - ink_top(&ui_font_bold_24));
    } else if (ui.p.wifi_setup) {
        lv_label_set_text_fmt(m.pair_keys, "%s\n%s", s->pair_key_wifi, s->pair_key_cancel);
        lv_obj_set_y(m.pair_keys, (int32_t)ui.info.height - BAR_H - 2 * ui_font_regular_16.line_height - 6);
    } else {
        lv_label_set_text(m.pair_keys, s->pair_key_cancel);
        lv_obj_set_y(m.pair_keys, (int32_t)ui.info.height - BAR_H - ui_font_regular_16.line_height - 6);
    }
}

/* Draw `data` as a QR code into the A8 canvas (255 = ink), centred, whole pixels per module; the white
 * screen around it is the quiet zone. Cleared if it does not encode. */
static void draw_qr(const char *data)
{
    const size_t len = strlen(data);
    lv_draw_buf_clear(&qr_buf, NULL);
    lv_obj_invalidate(m.qr);
    if (len > sizeof(m.qr_tmp)) {
        return;
    }
    memcpy(m.qr_tmp, data, len);
    if (!qrcodegen_encodeBinary(m.qr_tmp, len, m.qr_code, qrcodegen_Ecc_MEDIUM, 1, QR_VERSION_MAX, qrcodegen_Mask_AUTO,
                                true)) {
        return;
    }
    const int n = qrcodegen_getSize(m.qr_code), scale = QR_PX / n, margin = (QR_PX - n * scale) / 2;
    for (int y = 0; y < n * scale; y++) {
        uint8_t *row = lv_draw_buf_goto_xy(&qr_buf, 0, (uint32_t)(margin + y));
        for (int x = 0; x < n * scale; x++) {
            row[margin + x] = qrcodegen_getModule(m.qr_code, x / scale, y / scale) ? 0xFF : 0x00;
        }
    }
}

/* The Wi-Fi setup screen: the hotspot's QR code with its name, password and page address; then the outcome. */
static void apply_setup(void)
{
    const ui_strings_t *s = ui_strings();
    const ui_wifi_setup_t *st = &ui.setup;
    const bool waiting = st->state == UI_SETUP_WAITING;
    char buf[160];
    ui_set_hidden(m.qr, !waiting);
    if (waiting) {
        char data[sizeof(m.qr_data)];
        ui_wifi_qr_data(data, sizeof(data), st->ap_ssid, st->ap_password);
        if (strcmp(data, m.qr_data) != 0) {
            memcpy(m.qr_data, data, sizeof(data));
            draw_qr(data);
        }
        snprintf(buf, sizeof(buf), s->setup_join_short, st->ap_ssid, st->ap_password, st->url);
        lv_obj_set_pos(m.setup_text, MARGIN, QR_Y + QR_PX + 4);
        lv_obj_set_height(m.setup_text, 3 * ui_font_regular_16.line_height);
    } else {
        snprintf(buf, sizeof(buf),
                 st->state == UI_SETUP_JOINING ? s->setup_joining : st->state == UI_SETUP_DONE ? s->setup_done : s->setup_failed,
                 st->network);
        lv_obj_set_pos(m.setup_text, MARGIN, 56);
        lv_obj_set_height(m.setup_text, 6 * ui_font_regular_16.line_height);
    }
    ui_set_text(m.setup_text, buf);
    ui_set_text(m.setup_key, s->setup_key_close);
}

/* The settings list (D96, D97): the header names the list or page and says where the highlight is, the rows scroll
 * with it. The header is the only chrome: the whole panel below it is rows. */
static void apply_settings(void)
{
    const size_t n = ui_settings_count();
    const lv_color_t black = lv_color_black(), white = lv_color_white();
    char value[48], buf[24];
    lv_label_set_text(m.app, ui_settings_title());
    snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(ui.set_cur + 1), (unsigned)n);
    lv_label_set_text(m.counter, buf);
    for (size_t i = 0; i < UI_SETTINGS_ROWS; i++) {
        const size_t k = ui.set_top + i;
        const bool row = k < n, selected = row && k == ui.set_cur;
        ui_set_hidden(m.set_bg[i], !selected);
        ui_set_hidden(m.set_name[i], !row);
        ui_set_hidden(m.set_value[i], !row);
        if (!row) {
            continue;
        }
        const char *name;
        ui_settings_row(k, &name, value, sizeof(value));
        ui_set_text(m.set_name[i], name);
        ui_set_text(m.set_value[i], value);
        /* a long value (a network, the device name) leaves the label less room than the row */
        lv_point_t vs;
        lv_text_get_size(&vs, value, &ui_font_bold_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int32_t room = (int32_t)ui.info.width - 2 * (MARGIN + 2) - (vs.x ? vs.x + 8 : 0);
        lv_obj_set_width(m.set_name[i], room > 40 ? room : 40);
        lv_obj_set_style_text_color(m.set_name[i], selected ? white : black, 0);
        lv_obj_set_style_text_color(m.set_value[i], selected ? white : black, 0);
    }
}

/* Right side of the status bar: the charging bolt and "80%", the connection icon, the Wi-Fi fan; the left text
 * ends before them. */
static void apply_status_right(void)
{
    char buf[24] = "";
    if (ui.st.battery_pct <= 100) {
        snprintf(buf, sizeof(buf), "%u%%", ui.st.battery_pct);
    }
    lv_label_set_text(m.foot_right, buf);
    lv_point_t ts;
    lv_text_get_size(&ts, buf, &ui_font_regular_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const bool bolt = ui.powered && ui.st.external_power; /* charging: a guess from the voltage, PLAN D85 */
    int32_t right = (int32_t)ui.info.width - MARGIN - ts.x - (ts.x ? (bolt ? 2 : LINK_GAP) : 0);
    ui_set_hidden(m.bolt_icon.obj, !bolt);
    if (bolt) {
        right -= (int32_t)UI_STATUS_ICON_SIZE;
        icon_show(&m.bolt_icon, UI_STATUS_ICON_CHARGING, ui_status_icons[UI_STATUS_ICON_CHARGING], UI_STATUS_ICON_SIZE);
        lv_obj_set_pos(m.bolt_icon.obj, right,
                       (int32_t)ui.info.height - BAR_H + (BAR_H - (int32_t)UI_STATUS_ICON_SIZE) / 2);
        right -= LINK_GAP;
    }
    int32_t left_end = right;
    ui.link_src = UI_LINK_NONE;
    if (m.link) {
        ui_set_hidden(m.link, !ui.powered);
        if (ui.powered) {
            const ui_status_icon_t i = ui.st.connected ? UI_STATUS_ICON_BLE_CONNECTED : UI_STATUS_ICON_BLE_DISCONNECTED;
            load_mono1(m.link, m.link_buf, ui_status_icons[i], UI_STATUS_ICON_SIZE);
            left_end = right - (int32_t)UI_STATUS_ICON_SIZE;
            lv_obj_set_pos(m.link, left_end, (int32_t)ui.info.height - BAR_H + (BAR_H - (int32_t)UI_STATUS_ICON_SIZE) / 2);
            ui.link_src = ui.st.connected ? UI_LINK_CONNECTED : UI_LINK_DISCONNECTED;
        }
    }
    /* the Wi-Fi fan, left of the link icon: how the last join went; the radio is on only to fetch (D60) */
    ui.wifi_src = ui.powered ? ui_wifi_icon_for(ui.st.wifi_state) : UI_WIFI_NONE;
    if (m.wifi_icon.obj) {
        ui_set_hidden(m.wifi_icon.obj, ui.wifi_src == UI_WIFI_NONE);
        if (ui.wifi_src != UI_WIFI_NONE) {
            const ui_status_icon_t i = ui.wifi_src == UI_WIFI_FAILED ? UI_STATUS_ICON_WIFI_FAILED : UI_STATUS_ICON_WIFI;
            icon_show(&m.wifi_icon, (int)i, ui_status_icons[i], UI_STATUS_ICON_SIZE);
            left_end -= LINK_GAP + (int32_t)UI_STATUS_ICON_SIZE;
            lv_obj_set_pos(m.wifi_icon.obj, left_end, (int32_t)ui.info.height - BAR_H + (BAR_H - (int32_t)UI_STATUS_ICON_SIZE) / 2);
        }
    } else {
        ui.wifi_src = UI_WIFI_NONE;
    }
    lv_obj_set_width(m.foot_left, left_end - LINK_GAP - MARGIN);
}

static void apply(void)
{
    const ui_strings_t *s = ui_strings();
    char buf[48];
    const bool setup = ui.powered && ui.setup.state != UI_SETUP_OFF;
    const bool pairing = ui.powered && !setup && (ui.st.pairing_mode || ui.st.has_passkey);
    const bool settings = ui.powered && !setup && !pairing && ui.settings;
    const npipe_notif_t *n = ui.powered && !setup && !pairing && !settings && ui.n_cards && ui.show_cards
                                 ? &ui.cards[ui.cur]
                                 : NULL;
    const bool idle = ui.powered && !n && !setup && !pairing && !settings;
    ui_set_hidden(m.foot_left, !ui.powered || setup || settings);
    ui_set_hidden(m.foot_right, !ui.powered || setup || settings);
    ui_set_hidden(m.bar, !ui.powered || idle || setup || pairing); /* the cards and the settings have a header */
    ui_set_hidden(m.rule, !ui.powered || idle || setup || settings);
    if (!settings) { /* apply_settings shows its rows again */
        for (size_t i = 0; i < UI_SETTINGS_ROWS; i++) {
            ui_set_hidden(m.set_bg[i], true);
            ui_set_hidden(m.set_name[i], true);
            ui_set_hidden(m.set_value[i], true);
        }
    }
    ui_set_hidden(m.clock_box, !idle);
    ui_set_hidden(m.clock, !idle);
    ui_set_hidden(m.ampm, !idle);
    ui_set_hidden(m.date, !idle);
    if (!idle) { /* apply_idle shows its rows again */
        lv_obj_t *rows[] = {m.wx_icon.obj, m.wx_hilo, m.rain_icon.obj, m.wx_rain, m.divider,
                            m.temp_icon.obj, m.temp, m.rh_icon.obj, m.rh};
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
            ui_set_hidden(rows[i], true);
        }
    }
    /* at most UI_MAX_CARDS: one digit; the footer's left text (a software update) takes its place */
    const bool badge = idle && ui.n_cards > 0 && ui.st.ota_pct < 0;
    ui_set_hidden(m.badge, !badge);
    if (badge) {
        lv_label_set_text_fmt(m.badge, "%u", (unsigned)ui.n_cards);
    }
    ui_set_hidden(m.title, n == NULL);
    ui_set_hidden(m.body, n == NULL);
    ui_set_hidden(m.pair_title, !pairing);
    ui_set_hidden(m.pair_text, !pairing);
    ui_set_hidden(m.pair_keys, !pairing);
    ui_set_hidden(m.pair_box, true);
    ui_set_hidden(m.pair_code, true);
    ui_set_hidden(m.qr, true);
    ui_set_hidden(m.setup_text, !setup);
    ui_set_hidden(m.setup_key, !setup);
    ui_set_hidden(m.off, ui.powered);
    /* a message takes the footer for a few seconds */
    const bool notice = ui.powered && ui.notice != UI_NOTICE_NONE && !settings; /* the list shows the change itself */
    ui_set_hidden(m.notice_bar, !notice);
    ui_set_hidden(m.notice, !notice);
    if (notice) {
        lv_label_set_text(m.notice, ui.notice == UI_NOTICE_WIFI ? s->notice_wifi : s->notice_setting);
    }
    ui.icon_src = UI_ICON_NONE;
    if (m.icon) {
        lv_obj_add_flag(m.icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (!ui.powered) {
        lv_label_set_text_fmt(m.off, "ePager\n%s", s->off);
        apply_status_right(); /* hides the icon */
        return;
    }

    if (setup) {
        apply_setup();
    } else if (pairing) {
        apply_pairing();
    } else if (settings) {
        apply_settings();
    } else if (!n) {
        lv_label_set_text(m.app, "ePager");
        lv_label_set_text(m.counter, "");
        apply_idle();
    } else {
        lv_label_set_text(m.app, n->app_name_len ? n->app_name : n->app_id);
        snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(ui.cur + 1), (unsigned)ui.n_cards);
        lv_label_set_text(m.counter, buf);
        static char title_text[NPIPE_TITLE_MAX + 1], body_text[NPIPE_SUBTITLE_MAX + NPIPE_BODY_MAX + 2];
        lv_label_set_text(m.title, ui_card_text(title_text, sizeof(title_text), n->title_len ? n->title : n->subtitle));
        if (!ui.cfg.show_body) {
            lv_label_set_text(m.body, "");
        } else if (n->title_len && n->subtitle_len) {
            snprintf(body_text, sizeof(body_text), "%s\n%s", n->subtitle, n->body);
            lv_label_set_text(m.body, ui_card_text(body_text, sizeof(body_text), body_text));
        } else {
            lv_label_set_text(m.body, ui_card_text(body_text, sizeof(body_text), n->body));
        }
    }

    const bool with_icon = n && m.icon;
    if (with_icon) {
        show_icon(n);
        lv_obj_remove_flag(m.icon, LV_OBJ_FLAG_HIDDEN);
    }
    layout_text(with_icon);

    if (ui.st.ota_pct >= 0) {
        lv_label_set_text_fmt(m.foot_left, s->updating, ui.st.ota_pct);
    } else if (idle && ui.upd.mark) {
        lv_label_set_text(m.foot_left, s->upd_mark); /* a firmware update is waiting (the page opens from the settings) */
    } else if (idle || pairing || settings) {
        lv_label_set_text(m.foot_left, ""); /* the time is the big clock; the pairing screen has none */
    } else if (ui.utc) {
        const char *ampm;
        ui_format_time(buf, sizeof(buf), &ampm, true);
        if (ampm) {
            strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, ampm, sizeof(buf) - strlen(buf) - 1);
        }
        lv_label_set_text(m.foot_left, buf);
    } else {
        lv_label_set_text(m.foot_left, "--:--");
    }
    apply_status_right();
    if (setup || settings || notice) {
        ui_set_hidden(m.link, true); /* the hint, or the message, takes the footer */
        ui_set_hidden(m.wifi_icon.obj, true);
        ui_set_hidden(m.bolt_icon.obj, true);
    }
    if (notice) {
        ui_set_hidden(m.foot_left, true);
        ui_set_hidden(m.foot_right, true);
        ui_set_hidden(m.rule, true);
        ui_set_hidden(m.badge, true);
        ui_set_hidden(m.setup_key, true);
    }
}

static size_t visible_text(char *out, size_t cap)
{
    lv_obj_t *labels[24 + 2 * UI_SETTINGS_ROWS] = {m.app,     m.counter,  m.clock,    m.ampm,       m.date,     m.wx_hilo,   m.wx_rain,
                          m.temp,    m.rh,       m.badge,    m.title,      m.body,     m.pair_title, m.pair_text,
                          m.pair_code, m.pair_keys, m.setup_text, m.setup_key, m.notice, m.foot_left, m.foot_right, m.off};
    const size_t fixed = 22;
    for (size_t i = 0; i < UI_SETTINGS_ROWS; i++) { /* the settings rows, after the header's two labels in reading order */
        labels[fixed + 2 * i] = m.set_name[i];
        labels[fixed + 2 * i + 1] = m.set_value[i];
    }
    size_t n = 0;
    if (!cap) {
        return 0;
    }
    out[0] = 0;
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]) && ui.disp; i++) {
        lv_obj_t *l = labels[i];
        if (!l || lv_obj_has_flag(l, LV_OBJ_FLAG_HIDDEN) || (lv_obj_get_parent(l) && lv_obj_has_flag(lv_obj_get_parent(l), LV_OBJ_FLAG_HIDDEN))) {
            continue;
        }
        int k = snprintf(out + n, cap - n, "%s\n", lv_label_get_text(l));
        if (k < 0 || (size_t)k >= cap - n) {
            n = cap - 1;
            break;
        }
        n += (size_t)k;
    }
    return n;
}

static void reset(void) { memset(&m, 0, sizeof(m)); }

const ui_layout_t ui_layout_mono = {.build = build, .apply = apply, .visible_text = visible_text, .reset = reset};
