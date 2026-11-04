#include <stdio.h>
#include <string.h>

#include "epager/clock_face.h"

static const char *const WDAY_SHORT[2][7] = {{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
                                             {"CN", "T2", "T3", "T4", "T5", "T6", "T7"}};
static const char *const WDAY_LONG[2][7] = {
    {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
    {"Ch\xe1\xbb\xa7 nh\xe1\xba\xadt", "Th\xe1\xbb\xa9 Hai", "Th\xe1\xbb\xa9 Ba", "Th\xe1\xbb\xa9 T\xc6\xb0",
     "Th\xe1\xbb\xa9 N\xc4\x83m", "Th\xe1\xbb\xa9 S\xc3\xa1u", "Th\xe1\xbb\xa9 B\xe1\xba\xa3y"}};
static const char *const MONTH_SHORT[2][12] = {
    {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"},
    {"T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8", "T9", "T10", "T11", "T12"}};
static const char *const MONTH_LONG[2][12] = {
    {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November",
     "December"},
    {"Th\xc3\xa1ng 1", "Th\xc3\xa1ng 2", "Th\xc3\xa1ng 3", "Th\xc3\xa1ng 4", "Th\xc3\xa1ng 5", "Th\xc3\xa1ng 6",
     "Th\xc3\xa1ng 7", "Th\xc3\xa1ng 8", "Th\xc3\xa1ng 9", "Th\xc3\xa1ng 10", "Th\xc3\xa1ng 11", "Th\xc3\xa1ng 12"}};

#define DEGREE "\xc2\xb0"

/* The result of a source: text (has_text) and the number an arc fills or a condition tests (has_num). */
typedef struct {
    bool has_text, has_num;
    int32_t num;
    char text[CF_TEXT_MAX];
} src_t;

static void set_text(src_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>
static void set_text(src_t *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->text, sizeof(s->text), fmt, ap);
    va_end(ap);
    s->has_text = true;
}

static void set_num(src_t *s, int32_t n)
{
    s->has_num = true;
    s->num = n;
}

static int32_t round10(int32_t v) { return (v >= 0 ? v + 5 : v - 5) / 10; }

/* Whole degrees of the shown unit from tenths of a degree Celsius. */
static int32_t shown_c10(int32_t c10, bool fahrenheit)
{
    if (fahrenheit) {
        c10 = c10 * 9 / 5 + 320;
    }
    return round10(c10);
}

static const char *unit_of(const cf_data_t *d) { return d->fahrenheit ? DEGREE "F" : DEGREE "C"; }

static void weather_temp(src_t *s, bool has, int16_t c10, uint8_t fmt, const cf_data_t *d)
{
    if (!has) {
        return;
    }
    const int32_t v = shown_c10(c10, d->fahrenheit);
    set_text(s, "%d%s", (int)v, fmt == PROTO_FACE_FORMAT_NOUNIT ? "" : unit_of(d));
    set_num(s, v);
}

static void source_eval(src_t *s, uint8_t src, uint8_t fmt, const cf_data_t *d)
{
    const unsigned lang = d->language == PROTO_LANGUAGE_VI ? 1u : 0u;
    const bool pad2 = fmt == PROTO_FACE_FORMAT_PAD2, plain = fmt == PROTO_FACE_FORMAT_PLAIN;
    const bool unit = fmt != PROTO_FACE_FORMAT_NOUNIT;
    memset(s, 0, sizeof(*s));
    switch (src) {
    case PROTO_FACE_SOURCE_HOUR:
        if (!d->time_known) {
            set_text(s, "--");
            break;
        }
        {
            int h = d->hour;
            bool padded = !plain;
            if (!d->clock_24h) {
                h = h % 12 ? h % 12 : 12;
                padded = pad2;
            }
            set_text(s, padded ? "%02d" : "%d", h);
            set_num(s, h);
        }
        break;
    case PROTO_FACE_SOURCE_MINUTE:
        if (!d->time_known) {
            set_text(s, "--");
            break;
        }
        set_text(s, plain ? "%d" : "%02d", d->minute);
        set_num(s, d->minute);
        break;
    case PROTO_FACE_SOURCE_AMPM:
        if (d->time_known && !d->clock_24h) {
            set_text(s, "%s", d->hour < 12 ? "AM" : "PM");
        }
        break;
    case PROTO_FACE_SOURCE_TIME:
        if (!d->time_known) {
            set_text(s, "--:--");
        } else if (!d->clock_24h) {
            set_text(s, "%d:%02d", d->hour % 12 ? d->hour % 12 : 12, d->minute);
        } else {
            set_text(s, "%02d:%02d", d->hour, d->minute);
        }
        break;
    case PROTO_FACE_SOURCE_DAY:
        if (d->time_known) {
            set_text(s, pad2 ? "%02d" : "%d", d->day);
            set_num(s, d->day);
        }
        break;
    case PROTO_FACE_SOURCE_MONTH:
        if (!d->time_known || d->month < 1 || d->month > 12) {
            break;
        }
        if (fmt == PROTO_FACE_FORMAT_SHORT) {
            set_text(s, "%s", MONTH_SHORT[lang][d->month - 1]);
        } else if (fmt == PROTO_FACE_FORMAT_LONG) {
            set_text(s, "%s", MONTH_LONG[lang][d->month - 1]);
        } else {
            set_text(s, pad2 ? "%02d" : "%d", d->month);
        }
        set_num(s, d->month);
        break;
    case PROTO_FACE_SOURCE_YEAR:
        if (d->time_known) {
            set_text(s, fmt == PROTO_FACE_FORMAT_YEAR2 ? "%02d" : "%d", fmt == PROTO_FACE_FORMAT_YEAR2 ? d->year % 100 : d->year);
            set_num(s, d->year);
        }
        break;
    case PROTO_FACE_SOURCE_WEEKDAY:
        if (d->time_known && d->wday < 7) {
            set_text(s, "%s", (fmt == PROTO_FACE_FORMAT_LONG ? WDAY_LONG : WDAY_SHORT)[lang][d->wday]);
        }
        break;
    case PROTO_FACE_SOURCE_DATE: set_text(s, "%s", d->date ? d->date : ""); break;
    case PROTO_FACE_SOURCE_WEATHER_TEMP: weather_temp(s, d->has_wx_temp, d->wx_temp_c10, fmt, d); break;
    case PROTO_FACE_SOURCE_WEATHER_HIGH: weather_temp(s, d->has_wx_high, d->wx_high_c10, fmt, d); break;
    case PROTO_FACE_SOURCE_WEATHER_LOW: weather_temp(s, d->has_wx_low, d->wx_low_c10, fmt, d); break;
    case PROTO_FACE_SOURCE_WEATHER_RAIN:
        if (d->has_wx_rain && d->wx_rain <= 100) {
            set_text(s, unit ? "%d%%" : "%d", d->wx_rain);
            set_num(s, d->wx_rain);
        }
        break;
    case PROTO_FACE_SOURCE_WEATHER_CONDITION:
        if (d->has_wx_cond) {
            set_num(s, d->wx_cond);
        }
        break;
    case PROTO_FACE_SOURCE_WEATHER_PLACE:
        if (d->wx_place && d->wx_place[0]) {
            set_text(s, "%s", d->wx_place);
        }
        break;
    case PROTO_FACE_SOURCE_ROOM_TEMP:
        if (d->has_room) {
            int32_t centi = d->room_temp_c100;
            if (d->fahrenheit) {
                centi = centi * 9 / 5 + 3200;
            }
            const int32_t t10 = round10(centi), a = t10 < 0 ? -t10 : t10;
            set_text(s, "%s%d.%d%s", t10 < 0 ? "-" : "", (int)(a / 10), (int)(a % 10), unit ? unit_of(d) : "");
            set_num(s, t10 / 10);
        }
        break;
    case PROTO_FACE_SOURCE_ROOM_HUMIDITY:
        if (d->has_room) {
            const int32_t rh = ((int32_t)d->room_rh_c100 + 50) / 100;
            set_text(s, unit ? "%d%%" : "%d", (int)rh);
            set_num(s, rh);
        }
        break;
    case PROTO_FACE_SOURCE_BATTERY_PCT:
        if (d->battery_pct <= 100) {
            set_text(s, unit ? "%d%%" : "%d", d->battery_pct);
            set_num(s, d->battery_pct);
        }
        break;
    case PROTO_FACE_SOURCE_NOTIF_COUNT:
        set_text(s, "%u", (unsigned)d->notif_count);
        set_num(s, d->notif_count);
        break;
    case PROTO_FACE_SOURCE_CHARGING: set_num(s, d->charging); break;
    case PROTO_FACE_SOURCE_BLE_CONNECTED: set_num(s, d->ble); break;
    case PROTO_FACE_SOURCE_WIFI_CONNECTED: set_num(s, d->wifi); break;
    case PROTO_FACE_SOURCE_UPDATE_AVAILABLE: set_num(s, d->update_available); break;
    default: break;
    }
}

static bool is_bool_source(uint8_t src)
{
    return src == PROTO_FACE_SOURCE_CHARGING || src == PROTO_FACE_SOURCE_BLE_CONNECTED ||
           src == PROTO_FACE_SOURCE_WIFI_CONNECTED || src == PROTO_FACE_SOURCE_UPDATE_AVAILABLE;
}

static bool is_numeric_source(uint8_t src)
{
    switch (src) {
    case PROTO_FACE_SOURCE_HOUR: case PROTO_FACE_SOURCE_MINUTE: case PROTO_FACE_SOURCE_DAY:
    case PROTO_FACE_SOURCE_MONTH: case PROTO_FACE_SOURCE_YEAR: case PROTO_FACE_SOURCE_WEATHER_TEMP:
    case PROTO_FACE_SOURCE_WEATHER_HIGH: case PROTO_FACE_SOURCE_WEATHER_LOW: case PROTO_FACE_SOURCE_WEATHER_RAIN:
    case PROTO_FACE_SOURCE_ROOM_TEMP: case PROTO_FACE_SOURCE_ROOM_HUMIDITY: case PROTO_FACE_SOURCE_BATTERY_PCT:
    case PROTO_FACE_SOURCE_NOTIF_COUNT:
        return true;
    default: return false;
    }
}

static bool source_has_value(uint8_t src, const cf_data_t *d)
{
    if (is_bool_source(src)) {
        return true;
    }
    if (src == PROTO_FACE_SOURCE_HOUR || src == PROTO_FACE_SOURCE_MINUTE || src == PROTO_FACE_SOURCE_TIME) {
        return d->time_known;
    }
    src_t s;
    source_eval(&s, src, 0, d);
    return s.has_text || s.has_num;
}

static bool source_gt0(uint8_t src, const cf_data_t *d)
{
    if (!is_numeric_source(src) && !is_bool_source(src)) {
        return false;
    }
    src_t s;
    source_eval(&s, src, 0, d);
    return s.has_num && s.num > 0;
}

static bool cond_holds(const cf_face_t *f, const uint8_t *values, const cf_cond_t *c, const cf_data_t *d)
{
    bool ok = false;
    switch (c->kind) {
    case PROTO_FACE_COND_OPTION_EQ:
        for (unsigned i = 0; i < f->n_options; i++) {
            if (f->option[i].id == c->option) {
                ok = values[i] == c->value;
            }
        }
        break;
    case PROTO_FACE_COND_SOURCE_HAS: ok = source_has_value(c->source, d); break;
    case PROTO_FACE_COND_SOURCE_GT0: ok = source_gt0(c->source, d); break;
    default: break;
    }
    return ok != c->negate;
}

static void glyphs(cf_out_t *o)
{
    const char *t = o->text;
    o->n_glyph = 0;
    for (size_t i = 0; t[i] && o->n_glyph < CF_GLYPH_MAX; i++) {
        const unsigned char ch = (unsigned char)t[i];
        int slot = -1;
        if ((ch == 'A' || ch == 'P') && t[i + 1] == 'M') {
            slot = ch == 'A' ? (int)CF_SLOT_AM : (int)CF_SLOT_PM;
            i++;
        } else if (ch >= '0' && ch <= '9') {
            slot = ch - '0';
        } else if (ch == ':') {
            slot = (int)CF_SLOT_COLON;
        } else if (ch == '-') {
            slot = (int)CF_SLOT_MINUS;
        } else if (ch == '%') {
            slot = (int)CF_SLOT_PERCENT;
        } else if (ch == 0xC2 && (unsigned char)t[i + 1] == 0xB0) {
            slot = (int)CF_SLOT_DEGREE;
            i++;
        }
        if (slot >= 0) {
            o->glyph[o->n_glyph++] = (uint8_t)slot;
        }
    }
}

static uint16_t element_colour(const cf_face_t *f, const uint8_t *values, const cf_element_t *e)
{
    if (e->has_colour_option) {
        for (unsigned i = 0; i < f->n_options; i++) {
            const cf_option_t *o = &f->option[i];
            if (o->id == e->colour_option && o->type == PROTO_FACE_OPTION_TYPE_COLOUR && values[i] < o->n_choices) {
                return o->choice[values[i]].colour;
            }
        }
    }
    return e->colour;
}

void cf_eval(const cf_face_t *f, const uint8_t values[CF_MAX_OPTIONS], const cf_data_t *d, size_t idx, cf_out_t *out)
{
    memset(out, 0, sizeof(*out));
    if (idx >= f->n_elements) {
        return;
    }
    const cf_element_t *e = &f->element[idx];
    switch (e->kind) {
    case PROTO_FACE_ELEMENT_RECT: case PROTO_FACE_ELEMENT_LINE: case PROTO_FACE_ELEMENT_ARC:
    case PROTO_FACE_ELEMENT_IMAGE: case PROTO_FACE_ELEMENT_NUMBER: case PROTO_FACE_ELEMENT_TEXT:
    case PROTO_FACE_ELEMENT_ICON:
        break;
    default: return;
    }
    for (unsigned i = 0; i < e->n_conds; i++) {
        if (!cond_holds(f, values, &e->cond[i], d)) {
            return;
        }
    }
    out->colour = element_colour(f, values, e);
    src_t s;
    if (e->kind == PROTO_FACE_ELEMENT_NUMBER || e->kind == PROTO_FACE_ELEMENT_TEXT) {
        if (e->source == 0) {
            memset(&s, 0, sizeof(s));
            if (e->literal[0]) {
                set_text(&s, "%s", e->literal);
            }
        } else {
            source_eval(&s, e->source, e->format, d);
        }
        if (!s.has_text || !s.text[0] || e->source == PROTO_FACE_SOURCE_WEATHER_CONDITION || is_bool_source(e->source)) {
            return;
        }
        if (e->kind == PROTO_FACE_ELEMENT_TEXT && e->font > PROTO_FACE_FONT_CLOCK) {
            return;
        }
        memcpy(out->text, s.text, sizeof(out->text));
        if (e->kind == PROTO_FACE_ELEMENT_NUMBER) {
            glyphs(out);
        }
    } else if (e->kind == PROTO_FACE_ELEMENT_ARC) {
        source_eval(&s, e->source, e->format, d);
        if (!s.has_num) {
            return;
        }
        out->value = s.num < 0 ? 0 : s.num > 100 ? 100 : s.num;
    } else if (e->kind == PROTO_FACE_ELEMENT_ICON) {
        switch (e->icon) {
        case PROTO_FACE_ICON_WEATHER:
            if (!d->has_wx_cond) {
                return;
            }
            out->value = d->wx_cond;
            out->aux = d->wx_day;
            break;
        case PROTO_FACE_ICON_BLE: out->value = d->ble; break;
        case PROTO_FACE_ICON_WIFI:
            if (!d->wifi) {
                return;
            }
            out->value = 1;
            break;
        case PROTO_FACE_ICON_CHARGING:
            if (!d->charging) {
                return;
            }
            out->value = 1;
            break;
        default: return;
        }
    }
    out->visible = true;
}
