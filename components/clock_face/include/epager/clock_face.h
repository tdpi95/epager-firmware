/*
 * Clock faces (contracts/asset-formats.md "Clock faces"): the bounded parser of a face file, the option values and
 * the evaluator that says what every element of a face draws for a snapshot of the board's state. Portable C11:
 * no ESP-IDF, FreeRTOS or LVGL headers, no allocation, no logging. Drawing is ui_common's job (ui_face.c).
 *
 * A parsed face points into the file buffer for its image pixels: keep the buffer while the face is in use.
 * Vectors: contracts/test-vectors/clock_faces.json (the Python reference is tools/epager_proto/clockface.py).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/proto_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CF_MAX_IMAGES 32u
#define CF_MAX_OPTIONS 8u
#define CF_MAX_CHOICES 8u
#define CF_MAX_ELEMENTS 48u
#define CF_MAX_SETS 8u
#define CF_MAX_CONDS 2u
#define CF_MAX_OPT_PAIRS 16u   /* ui.clock_face_opts holds at most 32 bytes */
#define CF_NAME_MAX 32u
#define CF_LABEL_MAX 24u
#define CF_LITERAL_MAX 24u
#define CF_ASSET_HEADER_LEN 16u
#define CF_SLOTS 16u           /* digit-set slots: 0-9 digits, then colon, AM, PM, minus, degree, percent */
#define CF_SLOT_COLON 10u
#define CF_SLOT_AM 11u
#define CF_SLOT_PM 12u
#define CF_SLOT_MINUS 13u
#define CF_SLOT_DEGREE 14u
#define CF_SLOT_PERCENT 15u

/* What one board accepts (CAPABILITIES.asset_support and the display). */
typedef struct {
    uint16_t width, height;       /* the logical screen size the face must be made for */
    uint32_t max_file;            /* bytes */
    uint32_t formats;             /* bit (1 << PROTO_ASSET_FORMAT_*) for each image format the board draws */
    bool arcs;                    /* the board draws arc elements */
} cf_limits_t;

typedef struct {
    uint8_t format;               /* PROTO_ASSET_FORMAT_MONO1 / RGB565 / RGB565A8 */
    uint16_t width, height;
    uint8_t set;                  /* digit set 1-8, 0 = a plain image */
    uint8_t slot;
    const uint8_t *data;          /* in the file buffer */
    uint32_t len;
} cf_image_t;

typedef struct {
    char label[2][CF_LABEL_MAX + 1];  /* [0] English, [1] Vietnamese ("" = use English) */
    uint16_t colour;
} cf_choice_t;

typedef struct {
    uint8_t id, type;             /* type: PROTO_FACE_OPTION_TYPE_*; an unknown value is kept (only its default is valid) */
    uint8_t def;
    uint8_t n_choices;
    char label[2][CF_LABEL_MAX + 1];
    cf_choice_t choice[CF_MAX_CHOICES];
} cf_option_t;

typedef struct {
    uint8_t kind;                 /* PROTO_FACE_COND_* */
    uint8_t option, value, source;
    bool negate;
} cf_cond_t;

typedef struct {
    uint8_t kind;                 /* PROTO_FACE_ELEMENT_*; unknown values are kept and never drawn */
    int16_t x, y;
    uint16_t w, h;
    uint8_t align;
    uint16_t colour;
    bool has_colour_option;
    uint8_t colour_option;
    uint8_t source, format, image, set, font, icon;
    int16_t spacing;
    uint8_t thickness, radius;
    bool fill;
    uint16_t arc_start, arc_end;
    char literal[CF_LITERAL_MAX + 1];
    uint8_t n_conds;
    cf_cond_t cond[CF_MAX_CONDS];
} cf_element_t;

typedef struct {
    uint16_t width, height;
    char id[CF_NAME_MAX + 1];
    char name[2][CF_NAME_MAX + 1];  /* [0] English, [1] Vietnamese ("" = use English) */
    char author[CF_NAME_MAX + 1];
    char version[CF_NAME_MAX + 1];
    uint8_t update_anchor;          /* PROTO_FACE_ANCHOR_* */
    uint8_t n_images, n_options, n_elements;
    cf_image_t image[CF_MAX_IMAGES];
    cf_option_t option[CF_MAX_OPTIONS];
    cf_element_t element[CF_MAX_ELEMENTS];
    uint8_t set_image[CF_MAX_SETS + 1][CF_SLOTS]; /* [set][slot] = image index + 1, 0 = none */
    uint16_t set_height[CF_MAX_SETS + 1];         /* height of the digits of a complete set, else 0 */
} cf_face_t;

/* Parse and validate a whole file once. `expected_id` is the asset id the file is installed under (NULL: not
 * checked). `contract` is the board's contract version packed as major << 16 | minor << 8 | patch. Returns PROTO_OK,
 * PROTO_TOO_LARGE (the file is above lim->max_file) or PROTO_IMAGE_INVALID; `*why` (may be NULL) then points to a
 * short reason for logs and tools. Never reads outside file[0, len). */
proto_status_t cf_parse(const uint8_t *file, size_t len, const cf_limits_t *lim, const char *expected_id,
                        uint32_t contract, cf_face_t *out, const char **why);

/* True if an element or a condition of the face reads `source` (PROTO_FACE_SOURCE_*). */
bool cf_draws_source(const cf_face_t *f, uint8_t source);

/* ---------------------------------------------------------------- options */

/* How many values an option has: 2 for a toggle, the number of choices, 0 for an unknown type. */
uint8_t cf_option_range(const cf_option_t *o);
/* One value per option of the face (table order) from stored (id, value) pairs: unknown ids are ignored, a value out of
 * range counts as the default, of a repeated id the last pair wins; an odd last byte is ignored. */
void cf_opts_resolve(const cf_face_t *f, const uint8_t *pairs, size_t n, uint8_t values[CF_MAX_OPTIONS]);
/* The pairs of the values that differ from their default, in table order; returns the length (cap >= 2 * options). */
size_t cf_opts_pack(const cf_face_t *f, const uint8_t values[CF_MAX_OPTIONS], uint8_t *out, size_t cap);

/* ---------------------------------------------------------------- evaluation */

/* The board's state at one moment, as a face's sources read it (the UI fills it: ui_face.c). */
typedef struct {
    bool time_known;
    uint8_t hour, minute, day, month, wday;   /* local time: hour 0-23, month 1-12, wday 0 = Sunday */
    uint16_t year;
    bool clock_24h;
    uint8_t language;                         /* PROTO_LANGUAGE_* */
    const char *date;                         /* the Classic date line, "Time not set" when unknown; NULL = "" */
    bool fahrenheit;                          /* ui.temp_unit */
    bool has_wx_temp, has_wx_high, has_wx_low;
    int16_t wx_temp_c10, wx_high_c10, wx_low_c10; /* tenths of a degree Celsius */
    bool has_wx_rain;
    uint8_t wx_rain;                          /* percent; above 100 = unknown */
    bool has_wx_cond;
    uint8_t wx_cond;                          /* wx_cond_t */
    bool wx_day;
    const char *wx_place;                     /* NULL or "" = none */
    bool has_room;                            /* the board has a sensor and a good reading */
    int16_t room_temp_c100;                   /* hundredths of a degree Celsius */
    uint16_t room_rh_c100;                    /* hundredths of a percent */
    uint8_t battery_pct;                      /* 255 = unknown */
    bool charging, ble, wifi;
    uint16_t notif_count;
    bool update_available;
} cf_data_t;

#define CF_TEXT_MAX 80u
#define CF_GLYPH_MAX 16u

/* What one element draws. */
typedef struct {
    bool visible;                 /* false: skip it (unknown kind, a false condition, a source without a value) */
    uint16_t colour;              /* RGB565 */
    int32_t value;                /* arc: 0-100; icon: weather condition, or 1/0 for ble, 1 for wifi and charging */
    uint8_t aux;                  /* weather icon: 1 = daytime */
    char text[CF_TEXT_MAX];       /* text and number: the string */
    uint8_t glyph[CF_GLYPH_MAX];  /* number: the digit-set slots to draw, left to right */
    uint8_t n_glyph;
} cf_out_t;

/* Evaluate element `idx` (< f->n_elements) with the option `values` of cf_opts_resolve. */
void cf_eval(const cf_face_t *f, const uint8_t values[CF_MAX_OPTIONS], const cf_data_t *d, size_t idx, cf_out_t *out);

#ifdef __cplusplus
}
#endif
