/*
 * The LCD screens (colour layout, PLAN D51, light theme: PLAN 7c), 296 x 240 RGB565:
 * - status bar: the time (or pairing / update state) on the left, Wi-Fi (the last join's outcome, D60),
 *   Bluetooth and battery on the right;
 * - notification card: a stripe in the category's accent colour, the app's colour icon (else the
 *   built-in category icon on a tinted tile), app name, position "2/5", title and wrapped body;
 * - idle screen: a large clock and the date, and with a weather reading a row under them: the
 *   condition icon, the temperature, today's high / low and the chance of rain (PLAN D64), and the
 *   place's name under it (the clock block moves up to make room);
 * - clock screen with a badge (a pill in the status bar's empty left part, in the newest card's colour) counting
 *   the notifications waiting while the cards are hidden (PLAN D70);
 * - settings (swipe up on the clock screen): the list of the Paper's settings, four rows a page, a tap on a row
 *   selects it (PLAN D99);
 * - pairing screen (pairing mode): how to connect, and "Wi-Fi setup" on boards with Wi-Fi (D61);
 * - Wi-Fi setup screen: the hotspot's QR code, name, password and page address, then the outcome.
 * Text: anti-aliased Noto Sans (4 bpp) with colour emoji as the fonts' fallback (PLAN D59).
 * Touch (PLAN D70, D99): on a card, swipe left/right = next/previous, swipe down = dismiss (UI_ACTION_DISMISS), swipe up =
 * back to the clock screen (the notifications stay), tap = the whole body (and back); on the clock screen, swipe
 * down = the cards, swipe up = settings; in the settings, tap = select, swipe up/down = next/previous page,
 * swipe right = back (a page of the list, then the clock screen); on the pairing screen, swipe down = leave pairing mode.
 */
#include <stdio.h>
#include <string.h>

#include "epager/blob_storage.h"
#include "epager/proto_gen.h"
#include "epager/xfer.h"
#include "src/libs/qrcode/qrcodegen.h"
#include "ui_icons.h"
#include "ui_priv.h"

LV_FONT_DECLARE(ui_font_lcd_16)
LV_FONT_DECLARE(ui_font_lcd_20)
LV_FONT_DECLARE(ui_font_lcd_bold_20)
LV_FONT_DECLARE(ui_font_lcd_clock_72)

#define FONT_SMALL (&ui_font_lcd_16)       /* status bar, app name, card position */
#define FONT_TEXT (&ui_font_lcd_20)        /* body */
#define FONT_BOLD (&ui_font_lcd_bold_20)   /* title, date */
#define FONT_CLOCK (&ui_font_lcd_clock_72)

#define STATUS_H 24
#define STRIPE_W 4
#define PAD 12
#define ICON 32                      /* app icon size: asset_support app_icon rgb565a8 32 x 32 */
#define ICON_RADIUS 8
#define HEAD_X (PAD + ICON + 8)
#define HEAD_Y (STATUS_H + 8)
#define CY(y) ((y) - STATUS_H)       /* a screen y inside the card container, which starts below the status bar */
#define APP_Y (HEAD_Y - 4)
#define TITLE_Y (HEAD_Y + 13)
#define BODY_Y (HEAD_Y + ICON + 14)
#define BODY_BOTTOM 236
#define CARD_END_PAD 12              /* below the last line of a scrolled card */
#define COUNTER_W 50
#define BADGE_W 24
#define BADGE_H 18
#define LINK_GAP 4
/* Status bar texts from the screen edges: our margin plus the board's inset, for its rounded corners (only the
 * bar reaches into them; the cards keep PAD). The widest case still fits
 * with a board inset of 14 (296 px wide): "Chế độ ghép nối" (124 px) beside Wi-Fi, Bluetooth and "⚡ 100%" leaves the
 * left text 125 px. */
#define BAR_INSET (PAD + (int32_t)ui.info.edge_inset)
#define AMPM_GAP 4
#define DATE_GAP 10
#define WX_GAP 14                    /* date to the weather row */
#define WX_ICON_GAP 6                /* weather icon to the temperature */
#define WX_TEXT_GAP 12               /* temperature to the details */
#define WX_PLACE_GAP 2               /* weather row to the place's name */
#define BTN_W 168
#define BTN_H 44
#define BTN_Y 172                    /* buttons: clear of the screen edges (P1a: touch 15-30 px inside) */
#define SET_TITLE_Y (STATUS_H + 2)
#define SET_ROW_Y (STATUS_H + 32)    /* first settings row; UI_TOUCH_ROWS of SET_ROW_H end at BTN_Y + BTN_H, clear of the edges */
#define SET_ROW_H 40
#define SET_VALUE_GAP 8
#define QR_SIZE 120
#define QR_VERSION_MAX 10            /* 57 modules: up to 213 bytes at ECC medium, far more than a Wi-Fi QR */
#define QR_BUF qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VERSION_MAX)
#define ICON_FILE_LEN (XFER_ASSET_HEADER_LEN + 3u * ICON * ICON)

/* Light theme */
#define C_BG 0xFFFFFF
#define C_BAR 0xF1F3F5
#define C_BAR_LINE 0xDDE1E6
#define C_TEXT 0x1D1F23
#define C_BODY 0x374151
#define C_MUTED 0x6B7280
#define C_BUTTON 0x2563EB
#define C_BUTTON_TEXT 0xFFFFFF
#define C_FAIL 0xDC2626

/* Accent per registry enum `category` (index = value; unknown values use `other`). */
static const uint32_t ACCENT[UI_BUILTIN_ICON_COUNT] = {
    0x6B7280, /* other */
    0x16A34A, /* incoming call */
    0xDC2626, /* missed call */
    0x7C3AED, /* voicemail */
    0x2563EB, /* social */
    0xEA580C, /* schedule */
    0x0891B2, /* email */
    0xCA8A04, /* news */
    0xDB2777, /* health and fitness */
    0x059669, /* business and finance */
    0x0D9488, /* location */
    0x9333EA, /* entertainment */
};

static struct {
    lv_obj_t *bar, *bar_line, *bar_left, *bar_right, *link, *wifi;
    lv_draw_buf_t *link_buf, *wifi_buf;
    lv_obj_t *stripe, *card_box, *tile, *builtin, *app_icon, *app, *counter, *title, *body;
    lv_draw_buf_t *builtin_buf, *app_buf;
    lv_obj_t *clock, *ampm, *date, *off;
    lv_obj_t *badge;                 /* clock screen: how many notifications wait (swipe down opens them) */
    int32_t clock_ink_y, clock_ink_y_wx, clock_ink_y_place; /* clock digits' top: no weather, the row, row + place */
    lv_obj_t *wx_icon, *wx_temp, *wx_detail, *wx_place;
    int wx_icon_shown;               /* ui_weather_icon_t on screen, -1 none */
    lv_obj_t *pair_title, *pair_hint, *pair_code, *pair_code_hint, *wifi_btn, *wifi_btn_label;
    lv_obj_t *qr, *setup_text, *close_btn, *close_label;
    lv_obj_t *set_box, *set_title, *set_page; /* only while the settings are open (settings_build) */
    lv_obj_t *set_row[UI_TOUCH_ROWS], *set_name[UI_TOUCH_ROWS], *set_value[UI_TOUCH_ROWS];
    char qr_data[160];               /* what the QR code holds now */
    uint8_t qr_tmp[QR_BUF], qr_code[QR_BUF];
    bool expanded;                   /* tap: the whole title and body, scrolled by a drag when they exceed the screen */
    bool wrapped;                    /* title and body are in the wrapping mode of the expanded card */
    bool scrolls;                    /* the expanded card is taller than the screen: vertical drags scroll it */
    uint32_t shown_id;               /* card on screen: a new card collapses the body again */
    uint32_t icon_id, icon_gen;      /* icons on screen: loaded again only when these change */
    int link_state, wifi_state;      /* -1 none, else the ui_status_icon_t shown */
} c;

/* The QR code's pixels: A8, which LVGL blends directly. Static, not in LVGL's heap: an A1 image would be
 * decoded into a 14.4 KB A8 copy at every draw, more than the colour UI leaves free (measured 2026-10-14:
 * about 31 KB of the 48 KB pool in use, largest free block 13.3 KB). */
static UI_EXT_BSS LV_ATTRIBUTE_MEM_ALIGN uint8_t qr_px[LV_DRAW_BUF_SIZE(QR_SIZE, QR_SIZE, LV_COLOR_FORMAT_A8)];
static lv_draw_buf_t qr_buf;
static lv_style_t row_style, row_pressed_style; /* the settings rows */

_Static_assert(UI_TOUCH_ROWS == 4, "visible_text lists the four settings rows by hand");

static lv_color_t hex(uint32_t rgb) { return lv_color_hex(rgb); }

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, int32_t x, int32_t y, int32_t w, int32_t h,
                       lv_label_long_mode_t mode, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_label_set_long_mode(l, mode);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, hex(color), 0);
    lv_label_set_text_static(l, "");
    return l;
}

static lv_obj_t *rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *canvas(lv_obj_t *parent, lv_draw_buf_t **buf, uint32_t size, lv_color_format_t cf)
{
    *buf = lv_draw_buf_create(size, size, cf, 0);
    if (!*buf) {
        return NULL;
    }
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(cv, *buf);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    return cv;
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

static int32_t ink_h(const lv_font_t *f)
{
    lv_font_glyph_dsc_t g;
    return lv_font_get_glyph_dsc(f, &g, '0', 0) ? (int32_t)g.box_h : f->line_height;
}

/* ---------------------------------------------------------------- touch */

/* The setup, pairing and settings screens cover the cards: swipes and taps do nothing to them. */
static bool setup_open(void) { return ui.powered && ui.setup.state != UI_SETUP_OFF; }
static bool pairing_open(void) { return ui.powered && !setup_open() && ui.st.pairing_mode; }
static bool settings_open(void) { return ui.powered && !setup_open() && !pairing_open() && ui.settings; }
static bool overlay(void) { return setup_open() || pairing_open() || settings_open(); }

static void on_gesture(lv_event_t *e)
{
    (void)e;
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (overlay()) {
        /* swipe down = leave pairing mode (the Wi-Fi setup has its own button); in the settings right = back, up and
         * down turn the pages */
        if (dir == LV_DIR_BOTTOM && pairing_open()) {
            ui_emit(UI_ACTION_PAIRING_TOGGLE, 0);
        } else if (settings_open()) {
            if (dir == LV_DIR_RIGHT) {
                ui_settings_back();
            } else if (dir == LV_DIR_TOP || dir == LV_DIR_BOTTOM) {
                ui_settings_scroll(dir == LV_DIR_TOP ? 1 : -1);
            }
        }
        lv_indev_wait_release(lv_indev_active());
        return;
    }
    /* PLAN D55/D70. Cards: left/right = next/previous, down = dismiss, up = back to the clock screen.
     * Clock screen: up = settings (D99), down = the cards (when there are any). */
    const npipe_notif_t *n = ui_current_card();
    if (n && c.scrolls && (dir == LV_DIR_TOP || dir == LV_DIR_BOTTOM)) {
        lv_indev_wait_release(lv_indev_active()); /* a drag past the end of a long card is not a dismiss */
        return;
    }
    switch (dir) {
    case LV_DIR_LEFT:
        if (n) {
            ui_step_card(1);
        }
        break;
    case LV_DIR_RIGHT:
        if (n) {
            ui_step_card(-1);
        }
        break;
    case LV_DIR_BOTTOM:
        if (n) {
            ui_emit(UI_ACTION_DISMISS, n->id); /* the card leaves when the core removes it */
        } else if (ui.powered && ui.n_cards) {
            ui.show_cards = true;
            ui.model_dirty = true;
        }
        break;
    case LV_DIR_TOP:
        if (n) {
            ui.show_cards = false; /* the notifications stay; the clock screen shows how many */
            ui.model_dirty = true;
        } else if (ui.powered) {
            ui_settings_enter();
        }
        break;
    default:
        break;
    }
    lv_indev_wait_release(lv_indev_active()); /* no click after a swipe */
}

static void on_click(lv_event_t *e)
{
    (void)e;
    if (!overlay() && ui_current_card()) {
        c.expanded = !c.expanded;
        ui.model_dirty = true;
    }
}

static void on_button(lv_event_t *e)
{
    ui_emit((ui_action_t)(uintptr_t)lv_event_get_user_data(e), 0);
}

static void on_row(lv_event_t *e)
{
    if (settings_open()) {
        ui_settings_tap(ui.set_top + (size_t)(uintptr_t)lv_event_get_user_data(e));
    }
}

/* ---------------------------------------------------------------- build */

static lv_obj_t *button(lv_obj_t *parent, lv_obj_t **text, ui_action_t action, int32_t y)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, BTN_W, BTN_H);
    lv_obj_set_pos(b, (ui.info.width - BTN_W) / 2, y);
    lv_obj_set_style_bg_color(b, hex(C_BUTTON), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, BTN_H / 2, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, on_button, LV_EVENT_CLICKED, (void *)(uintptr_t)action);
    *text = lv_label_create(b);
    lv_obj_set_style_text_font(*text, FONT_BOLD, 0);
    lv_obj_set_style_text_color(*text, hex(C_BUTTON_TEXT), 0);
    lv_label_set_text_static(*text, "");
    lv_obj_center(*text);
    return b;
}

/* The settings widgets exist only while the list is open: built for the first apply() that finds ui.settings set,
 * deleted when it closes. Always-present rows would cost the LVGL pool 3 KB, which its headroom (D37, the
 * test_lvgl_pool_headroom guard) does not have; open settings have no card to draw. */
static void settings_build(void)
{
    const int32_t w = ui.info.width, h = ui.info.height;
    lv_obj_t *box = lv_obj_create(lv_screen_active());
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE); /* only the rows are touched */
    c.set_box = box;
    c.set_title = label(box, FONT_BOLD, PAD, SET_TITLE_Y, w - 2 * PAD, FONT_BOLD->line_height, LV_LABEL_LONG_MODE_CLIP,
                        C_TEXT);
    lv_obj_set_style_text_align(c.set_title, LV_TEXT_ALIGN_CENTER, 0);
    c.set_page = label(box, FONT_SMALL, w - PAD - 40, SET_TITLE_Y + 6, 40, FONT_SMALL->line_height,
                       LV_LABEL_LONG_MODE_CLIP, C_MUTED);
    lv_obj_set_style_text_align(c.set_page, LV_TEXT_ALIGN_RIGHT, 0);
    for (size_t k = 0; k < UI_TOUCH_ROWS; k++) {
        lv_obj_t *r = lv_obj_create(box);
        lv_obj_set_pos(r, 0, SET_ROW_Y + (int32_t)k * SET_ROW_H);
        lv_obj_set_size(r, w, SET_ROW_H);
        lv_obj_add_style(r, &row_style, 0);
        lv_obj_add_style(r, &row_pressed_style, LV_STATE_PRESSED);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(r, on_row, LV_EVENT_CLICKED, (void *)(uintptr_t)k);
        const int32_t ty = (SET_ROW_H - FONT_TEXT->line_height) / 2;
        c.set_row[k] = r;
        c.set_name[k] = label(r, FONT_TEXT, PAD, ty, w - 2 * PAD, FONT_TEXT->line_height, LV_LABEL_LONG_MODE_DOTS, C_TEXT);
        c.set_value[k] = label(r, FONT_TEXT, w - PAD, ty, 0, FONT_TEXT->line_height, LV_LABEL_LONG_MODE_CLIP, C_BUTTON);
        lv_obj_set_style_text_align(c.set_value[k], LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static void settings_free(void)
{
    lv_obj_delete(c.set_box);
    c.set_box = c.set_title = c.set_page = NULL;
    for (size_t k = 0; k < UI_TOUCH_ROWS; k++) {
        c.set_row[k] = c.set_name[k] = c.set_value[k] = NULL;
    }
}

static void build(void)
{
    const int32_t w = ui.info.width, h = ui.info.height;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_style_init(&row_style); /* the settings rows share these: local styles would cost the pool about 300 bytes a row */
    lv_style_set_bg_opa(&row_style, LV_OPA_TRANSP);
    lv_style_set_radius(&row_style, 0);
    lv_style_set_pad_all(&row_style, 0);
    lv_style_set_border_color(&row_style, hex(C_BAR_LINE));
    lv_style_set_border_side(&row_style, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_width(&row_style, 1);
    lv_style_init(&row_pressed_style);
    lv_style_set_bg_color(&row_pressed_style, hex(C_BAR));
    lv_style_set_bg_opa(&row_pressed_style, LV_OPA_COVER);
    lv_obj_add_event_cb(scr, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr, on_click, LV_EVENT_CLICKED, NULL);

    /* status bar */
    const int32_t bar_text_y = (STATUS_H - (int32_t)FONT_SMALL->line_height) / 2;
    c.bar = rect(scr, 0, 0, w, STATUS_H, C_BAR);
    c.bar_line = rect(scr, 0, STATUS_H - 1, w, 1, C_BAR_LINE);
    c.bar_left = label(scr, FONT_SMALL, BAR_INSET, bar_text_y, w / 2 - BAR_INSET, FONT_SMALL->line_height,
                       LV_LABEL_LONG_MODE_CLIP, C_TEXT); /* width: up to the icons (apply_status) */
    c.bar_right = label(scr, FONT_SMALL, w / 2, bar_text_y, w / 2 - BAR_INSET, FONT_SMALL->line_height,
                        LV_LABEL_LONG_MODE_CLIP, C_TEXT);
    lv_obj_set_style_text_align(c.bar_right, LV_TEXT_ALIGN_RIGHT, 0);
    c.wifi = canvas(scr, &c.wifi_buf, UI_STATUS_ICON_SIZE, LV_COLOR_FORMAT_A1);
    if (c.wifi) {
        lv_obj_set_style_image_recolor_opa(c.wifi, LV_OPA_COVER, 0); /* colour: apply_status */
    }
    c.link = canvas(scr, &c.link_buf, UI_STATUS_ICON_SIZE, LV_COLOR_FORMAT_A1);
    if (c.link) {
        lv_obj_set_style_image_recolor(c.link, hex(C_TEXT), 0);
        lv_obj_set_style_image_recolor_opa(c.link, LV_OPA_COVER, 0);
    }

    /* the clock screen's badge: a pill in the status bar's left part (empty there), in the newest card's colour */
    c.badge = label(scr, FONT_SMALL, BAR_INSET, (STATUS_H - BADGE_H) / 2, BADGE_W, BADGE_H, LV_LABEL_LONG_MODE_CLIP,
                    C_BUTTON_TEXT);
    lv_obj_set_style_text_align(c.badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(c.badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c.badge, BADGE_H / 2, 0);
    lv_obj_set_style_pad_top(c.badge, (BADGE_H - (int32_t)FONT_SMALL->line_height) / 2, 0);
    lv_obj_remove_flag(c.badge, LV_OBJ_FLAG_CLICKABLE);

    /* card */
    c.stripe = rect(scr, 0, STATUS_H, STRIPE_W, h - STATUS_H, ACCENT[0]);
    /* the card's head, title and body sit in a container that a drag scrolls once a tap has shown all the text */
    lv_obj_t *box = c.card_box = lv_obj_create(scr);
    lv_obj_set_pos(box, 0, STATUS_H);
    lv_obj_set_size(box, w, h - STATUS_H);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE); /* a tap on the card reaches the screen's handler */
    c.tile = rect(box, PAD, CY(HEAD_Y), ICON, ICON, ACCENT[0]);
    lv_obj_set_style_radius(c.tile, ICON_RADIUS, 0);
    lv_obj_set_style_bg_opa(c.tile, LV_OPA_20, 0);
    c.builtin = canvas(box, &c.builtin_buf, UI_ICON_SIZE, LV_COLOR_FORMAT_A1);
    if (c.builtin) {
        lv_obj_set_pos(c.builtin, PAD + (ICON - (int32_t)UI_ICON_SIZE) / 2, CY(HEAD_Y) + (ICON - (int32_t)UI_ICON_SIZE) / 2);
        lv_obj_set_style_image_recolor_opa(c.builtin, LV_OPA_COVER, 0);
    }
    c.app_icon = canvas(box, &c.app_buf, ICON, LV_COLOR_FORMAT_RGB565A8);
    if (c.app_icon) {
        lv_obj_set_pos(c.app_icon, PAD, CY(HEAD_Y));
    }
    c.app = label(box, FONT_SMALL, HEAD_X, CY(APP_Y), w - HEAD_X - COUNTER_W - PAD, FONT_SMALL->line_height,
                  LV_LABEL_LONG_MODE_DOTS, C_MUTED);
    c.counter = label(box, FONT_SMALL, w - PAD - COUNTER_W, CY(APP_Y), COUNTER_W, FONT_SMALL->line_height,
                      LV_LABEL_LONG_MODE_CLIP, C_MUTED);
    lv_obj_set_style_text_align(c.counter, LV_TEXT_ALIGN_RIGHT, 0);
    c.title = label(box, FONT_BOLD, HEAD_X, CY(TITLE_Y), w - HEAD_X - PAD, FONT_BOLD->line_height,
                    LV_LABEL_LONG_MODE_DOTS, C_TEXT);
    c.body = label(box, FONT_TEXT, PAD, CY(BODY_Y), w - 2 * PAD, 0, LV_LABEL_LONG_MODE_DOTS, C_BODY);

    /* idle: the clock block is centred below the status bar, with or without the weather row */
    const int32_t content = ink_h(FONT_CLOCK) + DATE_GAP + ink_h(FONT_BOLD);
    c.clock_ink_y = STATUS_H + (h - STATUS_H - content) / 2;
    c.clock_ink_y_wx = STATUS_H + (h - STATUS_H - content - WX_GAP - (int32_t)UI_WEATHER_ICON_SIZE) / 2;
    c.clock_ink_y_place = c.clock_ink_y_wx - (WX_PLACE_GAP + FONT_SMALL->line_height) / 2;
    c.clock = label(scr, FONT_CLOCK, 0, 0, LV_SIZE_CONTENT, FONT_CLOCK->line_height, LV_LABEL_LONG_MODE_CLIP, C_TEXT);
    c.ampm = label(scr, FONT_BOLD, 0, 0, LV_SIZE_CONTENT, FONT_BOLD->line_height, LV_LABEL_LONG_MODE_CLIP, C_TEXT);
    c.date = label(scr, FONT_BOLD, PAD, 0, w - 2 * PAD, FONT_BOLD->line_height, LV_LABEL_LONG_MODE_DOTS, C_MUTED);
    lv_obj_set_style_text_align(c.date, LV_TEXT_ALIGN_CENTER, 0);
    c.wx_icon = lv_image_create(scr);
    lv_obj_remove_flag(c.wx_icon, LV_OBJ_FLAG_CLICKABLE);
    c.wx_temp = label(scr, FONT_BOLD, 0, 0, LV_SIZE_CONTENT, FONT_BOLD->line_height, LV_LABEL_LONG_MODE_CLIP, C_TEXT);
    c.wx_detail = label(scr, FONT_TEXT, 0, 0, LV_SIZE_CONTENT, FONT_TEXT->line_height, LV_LABEL_LONG_MODE_CLIP,
                        C_MUTED);
    c.wx_place = label(scr, FONT_SMALL, PAD, 0, w - 2 * PAD, FONT_SMALL->line_height, LV_LABEL_LONG_MODE_DOTS, C_MUTED);
    lv_obj_set_style_text_align(c.wx_place, LV_TEXT_ALIGN_CENTER, 0);

    /* pairing screen */
    c.pair_title = label(scr, FONT_BOLD, PAD, STATUS_H + 22, w - 2 * PAD, FONT_BOLD->line_height,
                         LV_LABEL_LONG_MODE_CLIP, C_TEXT);
    lv_obj_set_style_text_align(c.pair_title, LV_TEXT_ALIGN_CENTER, 0);
    c.pair_hint = label(scr, FONT_TEXT, PAD + 8, STATUS_H + 60, w - 2 * PAD - 16, 3 * FONT_TEXT->line_height,
                        LV_LABEL_LONG_MODE_WRAP, C_BODY);
    lv_obj_set_style_text_align(c.pair_hint, LV_TEXT_ALIGN_CENTER, 0);
    /* while a phone pairs: the passkey it asks for, large, with a line under it */
    c.pair_code = label(scr, FONT_CLOCK, PAD, STATUS_H + 52, w - 2 * PAD, FONT_CLOCK->line_height,
                        LV_LABEL_LONG_MODE_CLIP, C_TEXT);
    lv_obj_set_style_text_align(c.pair_code, LV_TEXT_ALIGN_CENTER, 0);
    c.pair_code_hint = label(scr, FONT_TEXT, PAD + 8, STATUS_H + 52 + FONT_CLOCK->line_height + 4, w - 2 * PAD - 16,
                             2 * FONT_TEXT->line_height, LV_LABEL_LONG_MODE_WRAP, C_BODY);
    lv_obj_set_style_text_align(c.pair_code_hint, LV_TEXT_ALIGN_CENTER, 0);
    c.wifi_btn = button(scr, &c.wifi_btn_label, UI_ACTION_WIFI_SETUP, BTN_Y);

    /* Wi-Fi setup screen: the QR code on the left, the steps on the right; the outcome centred */
    lv_draw_buf_init(&qr_buf, QR_SIZE, QR_SIZE, LV_COLOR_FORMAT_A8, 0, qr_px, sizeof(qr_px));
    lv_draw_buf_set_flag(&qr_buf, LV_IMAGE_FLAGS_MODIFIABLE);
    lv_draw_buf_clear(&qr_buf, NULL);
    c.qr = lv_canvas_create(scr);
    lv_canvas_set_draw_buf(c.qr, &qr_buf);
    lv_obj_remove_flag(c.qr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(c.qr, PAD + 4, STATUS_H + 16);
    lv_obj_set_style_image_recolor(c.qr, hex(0x000000), 0);
    lv_obj_set_style_image_recolor_opa(c.qr, LV_OPA_COVER, 0);
    c.setup_text = label(scr, FONT_SMALL, 0, 0, 0, 0, LV_LABEL_LONG_MODE_WRAP, C_TEXT);
    c.close_btn = button(scr, &c.close_label, UI_ACTION_WIFI_SETUP_CLOSE, BTN_Y);

    c.off = label(scr, FONT_BOLD, 0, h / 2 - FONT_BOLD->line_height, w, 2 * FONT_BOLD->line_height,
                  LV_LABEL_LONG_MODE_CLIP, C_TEXT);
    lv_obj_set_style_text_align(c.off, LV_TEXT_ALIGN_CENTER, 0);
}

/* ---------------------------------------------------------------- icons */

/* An installed `app_icon` + `rgb565a8` for this app (asset-formats.md), container checked again:
 * any mismatch means "no icon". The payload is LVGL's RGB565A8 layout, copied as it is. */
static bool load_app_icon(const npipe_notif_t *n)
{
    const blob_storage_t *b = ui.p.blob;
    char id[XFER_APP_ICON_ID_LEN + 1], name[BLOB_NAME_MAX + 1];
    uint8_t head[XFER_ASSET_HEADER_LEN];
    uint32_t size = 0;
    if (!b || !c.app_buf || c.app_buf->header.stride != ICON * 2) {
        return false;
    }
    xfer_app_icon_id(n->app_id, n->app_id_len, id);
    xfer_asset_blob_name(PROTO_ASSET_TYPE_APP_ICON, id, XFER_APP_ICON_ID_LEN, name);
    if (b->stat(b->ctx, name, &size) != BLOB_OK || size != ICON_FILE_LEN ||
        b->read(b->ctx, name, 0, head, sizeof(head)) != BLOB_OK) {
        return false;
    }
    const uint16_t w = (uint16_t)(head[8] | head[9] << 8), h = (uint16_t)(head[10] | head[11] << 8);
    const uint32_t payload = (uint32_t)head[12] | (uint32_t)head[13] << 8 | (uint32_t)head[14] << 16 |
                             (uint32_t)head[15] << 24;
    if (memcmp(head, "ESPA", 4) != 0 || head[4] != 1 || head[5] != PROTO_ASSET_TYPE_APP_ICON ||
        head[6] != PROTO_ASSET_FORMAT_RGB565A8 || head[7] != 0 || w != ICON || h != ICON ||
        payload != 3u * ICON * ICON ||
        b->read(b->ctx, name, XFER_ASSET_HEADER_LEN, c.app_buf->data, payload) != BLOB_OK) {
        return false;
    }
    lv_obj_invalidate(c.app_icon);
    return true;
}

/* Copy a mono1 icon (bit 1 = white) into an A1 canvas buffer (bit 1 = ink). */
static void load_mono1(lv_obj_t *cv, lv_draw_buf_t *buf, const uint8_t *src, uint32_t size)
{
    const uint32_t row_bytes = size / 8u;
    for (uint32_t y = 0; y < size; y++) {
        uint8_t *row = lv_draw_buf_goto_xy(buf, 0, y);
        for (uint32_t i = 0; i < row_bytes; i++) {
            row[i] = (uint8_t)~src[y * row_bytes + i];
        }
    }
    lv_obj_invalidate(cv);
}

static void show_icon(const npipe_notif_t *n, lv_color_t accent)
{
    if (n->id == c.icon_id && ui.assets_gen == c.icon_gen) {
        ui.icon_src = c.app_icon && !lv_obj_has_flag(c.app_icon, LV_OBJ_FLAG_HIDDEN) ? UI_ICON_APP : UI_ICON_BUILTIN;
        return; /* the same card's icon is already on screen */
    }
    c.icon_id = n->id;
    c.icon_gen = ui.assets_gen;
    const bool app = c.app_icon && load_app_icon(n);
    ui_set_hidden(c.app_icon, !app);
    ui_set_hidden(c.tile, app);
    if (c.builtin) {
        ui_set_hidden(c.builtin, app);
    }
    if (app) {
        ui.icon_src = UI_ICON_APP;
        return;
    }
    lv_obj_set_style_bg_color(c.tile, accent, 0);
    if (c.builtin) {
        lv_obj_set_style_image_recolor(c.builtin, accent, 0);
        load_mono1(c.builtin, c.builtin_buf,
                   ui_builtin_icons[n->category < UI_BUILTIN_ICON_COUNT ? n->category : PROTO_CATEGORY_FALLBACK],
                   UI_ICON_SIZE);
    }
    ui.icon_src = UI_ICON_BUILTIN;
}

/* ---------------------------------------------------------------- apply */

static void apply_status(bool idle)
{
    const ui_strings_t *s = ui_strings();
    char buf[48];
    const char *ampm;
    if (ui.st.pairing_mode) {
        ui_set_text(c.bar_left, s->pairing);
    } else if (ui.st.ota_pct >= 0) {
        snprintf(buf, sizeof(buf), s->updating, ui.st.ota_pct);
        ui_set_text(c.bar_left, buf);
    } else if (idle && ui.upd.mark) {
        ui_set_text(c.bar_left, s->upd_mark); /* a firmware update is waiting */
    } else if (idle || !ui.utc) {
        ui_set_text(c.bar_left, idle ? "" : "--:--"); /* the idle screen shows the big clock */
    } else {
        ui_format_time(buf, sizeof(buf), &ampm, true);
        if (ampm) {
            strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, ampm, sizeof(buf) - strlen(buf) - 1);
        }
        ui_set_text(c.bar_left, buf);
    }
    if (ui.st.battery_pct <= 100) {
        snprintf(buf, sizeof(buf), "%s%u%%", ui.st.external_power ? "\u26A1 " : "", ui.st.battery_pct);
    } else {
        snprintf(buf, sizeof(buf), "%s", ui.st.external_power ? "\u26A1" : "");
    }
    ui_set_text(c.bar_right, buf);

    /* right to left: power/battery label, Bluetooth, Wi-Fi; the left text gets the rest */
    lv_point_t ts;
    lv_text_get_size(&ts, buf, FONT_SMALL, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int32_t icon_y = (STATUS_H - (int32_t)UI_STATUS_ICON_SIZE) / 2;
    int32_t x = (int32_t)ui.info.width - BAR_INSET - ts.x; /* left edge of what is placed so far */
    ui.link_src = UI_LINK_NONE;
    if (c.link) {
        const ui_status_icon_t i = ui.st.connected ? UI_STATUS_ICON_BLE_CONNECTED : UI_STATUS_ICON_BLE_DISCONNECTED;
        if (c.link_state != (int)i) {
            c.link_state = (int)i;
            load_mono1(c.link, c.link_buf, ui_status_icons[i], UI_STATUS_ICON_SIZE);
        }
        x -= (ts.x ? LINK_GAP : 0) + (int32_t)UI_STATUS_ICON_SIZE;
        lv_obj_set_pos(c.link, x, icon_y);
        ui.link_src = ui.st.connected ? UI_LINK_CONNECTED : UI_LINK_DISCONNECTED;
    }
    ui.wifi_src = ui.powered ? ui_wifi_icon_for(ui.st.wifi_state) : UI_WIFI_NONE;
    if (c.wifi) {
        ui_set_hidden(c.wifi, ui.wifi_src == UI_WIFI_NONE);
        if (ui.wifi_src != UI_WIFI_NONE) {
            const ui_status_icon_t i = ui.wifi_src == UI_WIFI_FAILED ? UI_STATUS_ICON_WIFI_FAILED : UI_STATUS_ICON_WIFI;
            if (c.wifi_state != (int)i) {
                c.wifi_state = (int)i;
                load_mono1(c.wifi, c.wifi_buf, ui_status_icons[i], UI_STATUS_ICON_SIZE);
            }
            lv_obj_set_style_image_recolor(c.wifi, hex(ui.wifi_src == UI_WIFI_CONNECTING ? C_MUTED : C_TEXT), 0);
            x -= LINK_GAP + (int32_t)UI_STATUS_ICON_SIZE;
            lv_obj_set_pos(c.wifi, x, icon_y);
        }
    } else {
        ui.wifi_src = UI_WIFI_NONE;
    }
    lv_obj_set_width(c.bar_left, LV_MAX(x - LINK_GAP - BAR_INSET, 0));
}

static bool has_place(void) { return ui.wx_ok && ui.wx.place[0] != '\0'; }

/* The weather row, centred, its icon's top at `top`: icon, "31°", "32° / 25°   ☔86%"; the place's
 * name under it. */
static void apply_weather(int32_t top)
{
    lv_obj_t *row[] = {c.wx_icon, c.wx_temp, c.wx_detail};
    for (size_t i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
        ui_set_hidden(row[i], !ui.wx_ok);
    }
    ui_set_hidden(c.wx_place, !has_place());
    if (!ui.wx_ok) {
        return;
    }
    const wx_reading_t *r = &ui.wx;
    if (has_place()) {
        ui_set_text(c.wx_place, r->place);
        lv_obj_set_y(c.wx_place, top + (int32_t)UI_WEATHER_ICON_SIZE + WX_PLACE_GAP);
    }
    char temp[16], detail[48] = "";
    snprintf(temp, sizeof(temp), "%ld\u00B0", (long)ui_temp_shown(r->temp_c10));
    if (r->high_c10 != WX_TEMP_UNKNOWN && r->low_c10 != WX_TEMP_UNKNOWN) {
        snprintf(detail, sizeof(detail), "%ld\u00B0 / %ld\u00B0", (long)ui_temp_shown(r->high_c10),
                 (long)ui_temp_shown(r->low_c10));
    }
    if (r->precip_pct <= 100) {
        size_t n = strlen(detail);
        snprintf(detail + n, sizeof(detail) - n, "%s\u2614%u%%", n ? "   " : "", r->precip_pct);
    }
    ui_set_text(c.wx_temp, temp);
    ui_set_text(c.wx_detail, detail);
    const int icon = ui_weather_icon_for(r->cond, r->is_day);
    if (icon != c.wx_icon_shown) {
        c.wx_icon_shown = icon;
        if (icon >= 0) {
            lv_image_set_src(c.wx_icon, &ui_weather_icons[icon]);
        }
    }
    ui_set_hidden(c.wx_icon, icon < 0);

    lv_point_t ts, ds = {0, 0};
    lv_text_get_size(&ts, temp, FONT_BOLD, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (detail[0]) {
        lv_text_get_size(&ds, detail, FONT_TEXT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }
    const int32_t icon_w = icon >= 0 ? (int32_t)UI_WEATHER_ICON_SIZE + WX_ICON_GAP : 0;
    const int32_t width = icon_w + ts.x + (detail[0] ? WX_TEXT_GAP + ds.x : 0);
    int32_t x = (ui.info.width - width) / 2;
    lv_obj_set_pos(c.wx_icon, x, top);
    x += icon_w;
    /* the texts share a baseline, centred on the icon */
    const int32_t mid = top + (int32_t)UI_WEATHER_ICON_SIZE / 2;
    lv_obj_set_pos(c.wx_temp, x, mid - FONT_BOLD->line_height / 2);
    const int32_t baseline = mid - FONT_BOLD->line_height / 2 + FONT_BOLD->line_height - FONT_BOLD->base_line;
    lv_obj_set_pos(c.wx_detail, x + ts.x + WX_TEXT_GAP, baseline - (FONT_TEXT->line_height - FONT_TEXT->base_line));
}

static void apply_idle(void)
{
    char buf[48];
    const char *ampm = NULL;
    if (ui.utc) {
        ui_format_time(buf, sizeof(buf), &ampm, true);
        ui_set_text(c.clock, buf);
        ui_format_date(buf, sizeof(buf));
        ui_set_text(c.date, buf);
    } else {
        ui_set_text(c.clock, "--:--");
        ui_set_text(c.date, ui_strings()->no_time);
    }
    lv_point_t cs, as = {0, 0};
    lv_text_get_size(&cs, lv_label_get_text(c.clock), FONT_CLOCK, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    ui_set_hidden(c.ampm, ampm == NULL);
    if (ampm) {
        ui_set_text(c.ampm, ampm);
        lv_text_get_size(&as, ampm, FONT_BOLD, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        as.x += AMPM_GAP;
    }
    const int32_t ink_y = has_place() ? c.clock_ink_y_place : ui.wx_ok ? c.clock_ink_y_wx : c.clock_ink_y;
    const int32_t x = (ui.info.width - cs.x - as.x) / 2, y = ink_y - ink_top(FONT_CLOCK);
    lv_obj_set_pos(c.clock, x, y);
    const int32_t baseline = y + FONT_CLOCK->line_height - FONT_CLOCK->base_line;
    lv_obj_set_pos(c.ampm, x + cs.x + AMPM_GAP, baseline - (FONT_BOLD->line_height - FONT_BOLD->base_line));
    const int32_t date_ink_y = ink_y + ink_h(FONT_CLOCK) + DATE_GAP;
    lv_obj_set_y(c.date, date_ink_y - ink_top(FONT_BOLD));
    apply_weather(date_ink_y + ink_h(FONT_BOLD) + WX_GAP);
}

static void apply_card(const npipe_notif_t *n)
{
    char buf[16];
    const lv_color_t accent = hex(ACCENT[n->category < UI_BUILTIN_ICON_COUNT ? n->category : PROTO_CATEGORY_FALLBACK]);
    if (n->id != c.shown_id) {
        c.shown_id = n->id;
        c.expanded = false;
        lv_obj_scroll_to_y(c.card_box, 0, LV_ANIM_OFF);
    }
    if (!lv_color_eq(lv_obj_get_style_bg_color(c.stripe, 0), accent)) {
        lv_obj_set_style_bg_color(c.stripe, accent, 0);
    }
    show_icon(n, accent);
    ui_set_text(c.app, n->app_name_len ? n->app_name : n->app_id);
    snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(ui.cur + 1), (unsigned)ui.n_cards);
    ui_set_text(c.counter, ui.n_cards > 1 ? buf : "");
    static char title_text[NPIPE_TITLE_MAX + 1], body_text[NPIPE_SUBTITLE_MAX + NPIPE_BODY_MAX + 2];
    ui_set_text(c.title, ui_card_text(title_text, sizeof(title_text), n->title_len ? n->title : n->subtitle));
    if (!ui.cfg.show_body) {
        ui_set_text(c.body, "");
    } else if (n->title_len && n->subtitle_len) {
        snprintf(body_text, sizeof(body_text), "%s\n%s", n->subtitle, n->body);
        ui_set_text(c.body, ui_card_text(body_text, sizeof(body_text), body_text));
    } else {
        ui_set_text(c.body, ui_card_text(body_text, sizeof(body_text), n->body));
    }
    /* collapsed: one line of title and as many lines of body as fit above the foot, cut with dots.
     * expanded (tap): all of the title and body, the card scrolls when they are taller than the screen */
    const bool x = c.expanded;
    if (x != c.wrapped) {
        c.wrapped = x;
        lv_label_set_long_mode(c.title, x ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_long_mode(c.body, x ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_DOTS);
    }
    const int32_t w = ui.info.width;
    int32_t title_h = FONT_BOLD->line_height, body_y = BODY_Y;
    int32_t body_h = (BODY_BOTTOM - BODY_Y) / FONT_TEXT->line_height * FONT_TEXT->line_height;
    if (x) {
        lv_point_t ts, bs;
        lv_text_get_size(&ts, lv_label_get_text(c.title), FONT_BOLD, 0, 0, w - HEAD_X - PAD, LV_TEXT_FLAG_NONE);
        lv_text_get_size(&bs, lv_label_get_text(c.body), FONT_TEXT, 0, 0, w - 2 * PAD, LV_TEXT_FLAG_NONE);
        title_h = LV_MAX(ts.y, (int32_t)FONT_BOLD->line_height);
        body_y = LV_MAX(BODY_Y, TITLE_Y + title_h + 6);
        body_h = lv_label_get_text(c.body)[0] ? bs.y : 0;
    }
    lv_obj_set_height(c.title, title_h);
    lv_obj_set_y(c.body, CY(body_y));
    lv_obj_set_height(c.body, body_h);
    const int32_t end = CY(body_y) + body_h + (x ? CARD_END_PAD : 0);
    c.scrolls = x && end > (int32_t)ui.info.height - STATUS_H;
    lv_obj_set_style_pad_bottom(c.card_box, x ? CARD_END_PAD : 0, 0);
    if (!x) {
        lv_obj_scroll_to_y(c.card_box, 0, LV_ANIM_OFF);
    }
}

static void apply_pairing(void)
{
    const ui_strings_t *s = ui_strings();
    ui_set_text(c.pair_title, s->pairing);
    ui_set_text(c.pair_hint, s->pair_hint);
    char code[8];
    snprintf(code, sizeof(code), "%06u", (unsigned)(ui.st.passkey % 1000000u));
    ui_set_text(c.pair_code, code);
    ui_set_text(c.pair_code_hint, s->pair_code);
    ui_set_text(c.wifi_btn_label, s->wifi_setup);
    lv_obj_center(c.wifi_btn_label);
}

/* The rows of the page on screen: the name takes what the value leaves of the width. Rows past the end of the list
 * are hidden. */
static void apply_settings(void)
{
    const ui_strings_t *s = ui_strings();
    const size_t n = ui_settings_count();
    ui_set_text(c.set_title, ui_settings_title());
    char pages[24] = "";
    if (n > UI_TOUCH_ROWS) {
        snprintf(pages, sizeof(pages), "%u/%u", (unsigned)(ui.set_top / UI_TOUCH_ROWS + 1),
                 (unsigned)((n + UI_TOUCH_ROWS - 1) / UI_TOUCH_ROWS));
    }
    ui_set_text(c.set_page, pages);
    for (size_t k = 0; k < UI_TOUCH_ROWS; k++) {
        const size_t i = ui.set_top + k;
        ui_set_hidden(c.set_row[k], i >= n);
        if (i >= n) {
            continue;
        }
        const char *name;
        char value[24];
        ui_settings_row(i, &name, value, sizeof(value));
        ui_set_text(c.set_name[k], name);
        ui_set_text(c.set_value[k], value);
        lv_point_t sz = {0, 0};
        if (value[0]) {
            lv_text_get_size(&sz, value, FONT_TEXT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        }
        const int32_t vw = sz.x ? sz.x + 1 : 0;
        lv_obj_set_width(c.set_value[k], vw);
        lv_obj_set_x(c.set_value[k], ui.info.width - PAD - vw);
        lv_obj_set_width(c.set_name[k], ui.info.width - 2 * PAD - (vw ? vw + SET_VALUE_GAP : 0));
        lv_obj_set_style_text_color(c.set_value[k], hex(strcmp(value, s->forget_ask) == 0 ? C_FAIL : C_BUTTON), 0);
    }
}

/* Draw `data` as a QR code into the A8 canvas (255 = ink), centred, whole pixels per module; the white
 * screen around it is the quiet zone. Cleared if it does not encode. */
static void draw_qr(const char *data)
{
    const size_t len = strlen(data);
    lv_draw_buf_clear(&qr_buf, NULL);
    lv_obj_invalidate(c.qr);
    if (len > sizeof(c.qr_tmp)) {
        return;
    }
    memcpy(c.qr_tmp, data, len);
    if (!qrcodegen_encodeBinary(c.qr_tmp, len, c.qr_code, qrcodegen_Ecc_MEDIUM, 1, QR_VERSION_MAX,
                                qrcodegen_Mask_AUTO, true)) {
        return;
    }
    const int n = qrcodegen_getSize(c.qr_code), scale = QR_SIZE / n, margin = (QR_SIZE - n * scale) / 2;
    for (int y = 0; y < n * scale; y++) {
        uint8_t *row = lv_draw_buf_goto_xy(&qr_buf, 0, (uint32_t)(margin + y));
        for (int x = 0; x < n * scale; x++) {
            row[margin + x] = qrcodegen_getModule(c.qr_code, x / scale, y / scale) ? 0xFF : 0x00;
        }
    }
}

static void apply_setup(void)
{
    const ui_strings_t *s = ui_strings();
    const ui_wifi_setup_t *st = &ui.setup;
    const bool waiting = st->state == UI_SETUP_WAITING;
    char buf[160];
    ui_set_hidden(c.qr, !waiting);
    if (waiting) {
        char data[sizeof(c.qr_data)];
        ui_wifi_qr_data(data, sizeof(data), st->ap_ssid, st->ap_password);
        if (strcmp(data, c.qr_data) != 0) {
            memcpy(c.qr_data, data, sizeof(data));
            draw_qr(data);
        }
    }
    const int32_t w = ui.info.width;
    if (waiting) {
        const int32_t x = PAD + 4 + QR_SIZE + 12;
        lv_obj_set_pos(c.setup_text, x, STATUS_H + 10);
        lv_obj_set_size(c.setup_text, w - x - PAD, BTN_Y - STATUS_H - 14);
        lv_obj_set_style_text_align(c.setup_text, LV_TEXT_ALIGN_LEFT, 0);
        snprintf(buf, sizeof(buf), s->setup_join, st->ap_ssid, st->ap_password, st->url);
    } else {
        lv_obj_set_pos(c.setup_text, PAD, STATUS_H + 30);
        lv_obj_set_size(c.setup_text, w - 2 * PAD, BTN_Y - STATUS_H - 40);
        lv_obj_set_style_text_align(c.setup_text, LV_TEXT_ALIGN_CENTER, 0);
        snprintf(buf, sizeof(buf),
                 st->state == UI_SETUP_JOINING ? s->setup_joining : st->state == UI_SETUP_DONE ? s->setup_done : s->setup_failed,
                 st->network);
    }
    lv_obj_set_style_text_font(c.setup_text, waiting ? FONT_SMALL : FONT_BOLD, 0);
    lv_obj_set_style_text_color(c.setup_text, hex(st->state == UI_SETUP_FAILED ? C_FAIL : C_TEXT), 0);
    ui_set_text(c.setup_text, buf);
    ui_set_text(c.close_label, s->close);
    lv_obj_center(c.close_label);
}

static void apply(void)
{
    const bool setup = setup_open();
    const bool pairing = pairing_open();
    const bool settings = settings_open();
    const npipe_notif_t *n = setup || pairing || settings ? NULL : ui_current_card();
    const bool idle = ui.powered && !n && !setup && !pairing && !settings;
    lv_obj_t *bar[] = {c.bar, c.bar_line, c.bar_left, c.bar_right, c.link};
    lv_obj_t *card[] = {c.stripe, c.card_box, c.tile, c.builtin, c.app_icon, c.app, c.counter, c.title, c.body};
    lv_obj_t *idle_items[] = {c.clock, c.ampm, c.date};
    const bool badge = idle && ui.n_cards > 0;
    ui_set_hidden(c.badge, !badge);
    if (badge) {
        char num[8];
        snprintf(num, sizeof(num), "%u", (unsigned)ui.n_cards);
        ui_set_text(c.badge, num);
        const uint8_t cat = ui.cards[0].category;
        lv_obj_set_style_bg_color(c.badge, hex(ACCENT[cat < UI_BUILTIN_ICON_COUNT ? cat : PROTO_CATEGORY_FALLBACK]), 0);
    }
    lv_obj_t *pair_items[] = {c.pair_title, c.pair_hint};
    lv_obj_t *code_items[] = {c.pair_code, c.pair_code_hint};
    const bool code = pairing && ui.st.has_passkey; /* the passkey replaces the hint and the buttons */
    lv_obj_t *setup_items[] = {c.setup_text, c.close_btn};
    for (size_t i = 0; i < sizeof(bar) / sizeof(bar[0]); i++) {
        if (bar[i]) {
            ui_set_hidden(bar[i], !ui.powered);
        }
    }
    for (size_t i = 0; i < sizeof(card) / sizeof(card[0]); i++) {
        if (card[i]) {
            ui_set_hidden(card[i], n == NULL);
        }
    }
    for (size_t i = 0; i < sizeof(idle_items) / sizeof(idle_items[0]); i++) {
        ui_set_hidden(idle_items[i], !idle);
    }
    if (!idle) {
        lv_obj_t *row[] = {c.wx_icon, c.wx_temp, c.wx_detail, c.wx_place};
        for (size_t i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
            ui_set_hidden(row[i], true);
        }
    }
    for (size_t i = 0; i < sizeof(pair_items) / sizeof(pair_items[0]); i++) {
        ui_set_hidden(pair_items[i], !pairing);
    }
    for (size_t i = 0; i < sizeof(code_items) / sizeof(code_items[0]); i++) {
        ui_set_hidden(code_items[i], !code);
    }
    ui_set_hidden(c.pair_hint, !pairing || code);
    ui_set_hidden(c.wifi_btn, !(pairing && ui.p.wifi_setup) || code);
    for (size_t i = 0; i < sizeof(setup_items) / sizeof(setup_items[0]); i++) {
        ui_set_hidden(setup_items[i], !setup);
    }
    if (ui.settings && !c.set_box) {
        settings_build();
    } else if (!ui.settings && c.set_box) {
        settings_free();
    }
    if (c.set_box) { /* the setup and pairing screens are drawn under the rows' box: hide it while they show */
        ui_set_hidden(c.set_box, !settings);
    }
    if (!setup) {
        ui_set_hidden(c.qr, true);
    }
    ui_set_hidden(c.off, ui.powered);
    ui.icon_src = UI_ICON_NONE;
    ui.link_src = UI_LINK_NONE;
    if (!ui.powered) {
        char buf[48];
        snprintf(buf, sizeof(buf), "ePager\n%s", ui_strings()->off);
        ui_set_text(c.off, buf);
        return;
    }
    if (setup) {
        apply_setup();
    } else if (pairing) {
        apply_pairing();
    } else if (settings) {
        apply_settings();
    } else if (n) {
        apply_card(n);
    } else {
        apply_idle();
    }
    if (!n) {
        c.shown_id = 0;
    }
    apply_status(idle);
}

/* Neither the label nor a parent (a button) is hidden. */
static bool shown(lv_obj_t *o)
{
    for (; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
    }
    return true;
}

static size_t visible_text(char *out, size_t cap)
{
    lv_obj_t *labels[] = {c.bar_left,  c.bar_right, c.badge, c.app,       c.counter,        c.title,
                          c.body,      c.clock,     c.ampm,      c.date,           c.wx_temp,
                          c.wx_detail, c.wx_place, c.pair_title, c.pair_hint, c.pair_code, c.pair_code_hint, c.wifi_btn_label, c.setup_text,
                          c.close_label, c.set_title, c.set_page, c.set_name[0], c.set_value[0], c.set_name[1], c.set_value[1],
                          c.set_name[2], c.set_value[2], c.set_name[3], c.set_value[3], c.off};
    size_t n = 0;
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        lv_obj_t *l = labels[i];
        if (!l || !shown(l) || !lv_label_get_text(l)[0]) {
            continue; /* hidden or empty */
        }
        int k = snprintf(out + n, cap - n, "%s\n", lv_label_get_text(l));
        if (k < 0 || (size_t)k >= cap - n) {
            return cap - 1;
        }
        n += (size_t)k;
    }
    return n;
}

static void reset(void)
{
    memset(&c, 0, sizeof(c));
    c.link_state = c.wifi_state = -1;
    c.wx_icon_shown = -1;
}

const ui_layout_t ui_layout_colour = {.build = build, .apply = apply, .visible_text = visible_text, .reset = reset};
