/* Weather: response parsers, URL builders and the fetch scheduler (wx.h). */
#include "epager/wx.h"

#include <stdio.h>
#include <string.h>

#include "epager/proto_gen.h"

/* ---------------------------------------------------------------- conditions */

wx_cond_t wx_cond_from_wmo(int32_t code)
{
    switch (code) {
    case 0: return WX_COND_CLEAR;
    case 1: return WX_COND_MOSTLY_CLEAR;
    case 2: return WX_COND_PARTLY_CLOUDY;
    case 3: return WX_COND_OVERCAST;
    case 45: case 48: return WX_COND_FOG;
    case 51: case 53: case 55: case 56: case 57: return WX_COND_DRIZZLE;
    case 61: case 63: case 66: return WX_COND_RAIN;
    case 65: case 67: case 82: return WX_COND_HEAVY_RAIN;
    case 80: case 81: return WX_COND_SHOWERS;
    case 71: case 73: case 75: case 77: case 85: case 86: return WX_COND_SNOW;
    case 95: case 96: case 99: return WX_COND_THUNDER;
    default: return WX_COND_UNKNOWN;
    }
}

/* ---------------------------------------------------------------- parsers */

static bool get_fixed(const char *json, size_t len, const char *path, unsigned decimals, int32_t *out)
{
    wx_span_t v;
    return wx_json_get(json, len, path, &v) && wx_json_fixed(v, decimals, out);
}

static int16_t temp_or_unknown(const char *json, size_t len, const char *path)
{
    int32_t t;
    return get_fixed(json, len, path, 1, &t) && t > -1000 && t < 1000 ? (int16_t)t : WX_TEMP_UNKNOWN;
}

static bool is_api_error(const char *json, size_t len)
{
    wx_span_t v;
    return wx_json_get(json, len, "error", &v) && v.n == 4 && memcmp(v.p, "true", 4) == 0;
}

wx_result_t wx_parse_forecast(const char *json, size_t len, wx_reading_t *out)
{
    if (is_api_error(json, len)) {
        return WX_ERR_API;
    }
    int32_t temp, code, v;
    if (!get_fixed(json, len, "current.temperature_2m", 1, &temp) || temp <= -1000 || temp >= 1000 ||
        !get_fixed(json, len, "current.weather_code", 0, &code)) {
        return WX_ERR_FORMAT;
    }
    memset(out, 0, sizeof(*out));
    out->temp_c10 = (int16_t)temp;
    out->cond = (uint8_t)wx_cond_from_wmo(code);
    out->is_day = !get_fixed(json, len, "current.is_day", 0, &v) || v != 0;
    out->high_c10 = temp_or_unknown(json, len, "daily.temperature_2m_max[0]");
    out->low_c10 = temp_or_unknown(json, len, "daily.temperature_2m_min[0]");
    out->precip_pct = get_fixed(json, len, "daily.precipitation_probability_max[0]", 0, &v) && v >= 0 && v <= 100
                          ? (uint8_t)v : WX_PCT_UNKNOWN;
    /* the UTC offset of any place on Earth is within -12 h .. +14 h */
    out->has_utc_offset = get_fixed(json, len, "utc_offset_seconds", 0, &v) && v >= -12 * 3600 && v <= 14 * 3600;
    out->utc_offset_s = out->has_utc_offset ? v : 0;
    return WX_OK;
}

static bool valid_place(int32_t lat, int32_t lon) { return lat >= -900000 && lat <= 900000 && lon >= -1800000 && lon <= 1800000; }

wx_result_t wx_parse_geocode(const char *json, size_t len, wx_place_t *out)
{
    if (is_api_error(json, len)) {
        return WX_ERR_API;
    }
    wx_span_t v;
    if (!wx_json_get(json, len, "generationtime_ms", &v)) {
        return WX_ERR_FORMAT; /* every geocoding answer has it, with or without results */
    }
    if (!wx_json_get(json, len, "results[0]", &v)) {
        return WX_ERR_NOT_FOUND;
    }
    /* the most populous of the answers (a city over a village of the same name), else the first */
    int best = 0;
    int32_t best_pop = -1;
    for (int i = 0; i < WX_GEOCODE_COUNT; i++) {
        char path[32];
        snprintf(path, sizeof(path), "results[%d]", i);
        if (!wx_json_get(json, len, path, &v)) {
            break;
        }
        int32_t pop;
        snprintf(path, sizeof(path), "results[%d].population", i);
        if (get_fixed(json, len, path, 0, &pop) && pop > best_pop) {
            best_pop = pop;
            best = i;
        }
    }
    char lat_p[32], lon_p[32], name_p[32];
    snprintf(lat_p, sizeof(lat_p), "results[%d].latitude", best);
    snprintf(lon_p, sizeof(lon_p), "results[%d].longitude", best);
    snprintf(name_p, sizeof(name_p), "results[%d].name", best);
    int32_t lat, lon;
    if (!get_fixed(json, len, lat_p, 4, &lat) || !get_fixed(json, len, lon_p, 4, &lon) || !valid_place(lat, lon)) {
        return WX_ERR_FORMAT;
    }
    memset(out, 0, sizeof(*out));
    out->lat_e4 = lat;
    out->lon_e4 = lon;
    if (!wx_json_get(json, len, name_p, &v) || !wx_json_string(v, out->name, sizeof(out->name), NULL)) {
        out->name[0] = '\0';
    }
    return WX_OK;
}

wx_result_t wx_parse_ipinfo(const char *json, size_t len, wx_place_t *out)
{
    wx_span_t v;
    if (!wx_json_get(json, len, "ip", &v)) {
        return wx_json_get(json, len, "error", &v) ? WX_ERR_API : WX_ERR_FORMAT;
    }
    char loc[32];
    if (!wx_json_get(json, len, "loc", &v)) {
        return WX_ERR_NOT_FOUND; /* e.g. "bogon": a private address */
    }
    if (!wx_json_string(v, loc, sizeof(loc), NULL)) {
        return WX_ERR_FORMAT;
    }
    const char *comma = strchr(loc, ',');
    int32_t lat, lon;
    if (!comma || !wx_json_fixed((wx_span_t){loc, (size_t)(comma - loc)}, 4, &lat) ||
        !wx_json_fixed((wx_span_t){comma + 1, strlen(comma + 1)}, 4, &lon) || !valid_place(lat, lon)) {
        return WX_ERR_FORMAT;
    }
    memset(out, 0, sizeof(*out));
    out->lat_e4 = lat;
    out->lon_e4 = lon;
    if (!wx_json_get(json, len, "city", &v) || !wx_json_string(v, out->name, sizeof(out->name), NULL)) {
        out->name[0] = '\0';
    }
    return WX_OK;
}

/* ---------------------------------------------------------------- URLs */

static size_t fit(int n, size_t cap) { return n > 0 && (size_t)n < cap ? (size_t)n : 0; }

static int coord(char *out, size_t cap, int32_t e4)
{
    uint32_t a = e4 < 0 ? (uint32_t)0 - (uint32_t)e4 : (uint32_t)e4;
    return snprintf(out, cap, "%s%lu.%04lu", e4 < 0 ? "-" : "", (unsigned long)(a / 10000u), (unsigned long)(a % 10000u));
}

size_t wx_url_geocode(char *out, size_t cap, const uint8_t *city, size_t city_len, const char *lang)
{
    static const char hex[] = "0123456789ABCDEF";
    int n = snprintf(out, cap, "https://geocoding-api.open-meteo.com/v1/search?name=");
    if (fit(n, cap) == 0) {
        return 0;
    }
    size_t pos = (size_t)n;
    for (size_t i = 0; i < city_len; i++) {
        uint8_t b = city[i];
        bool keep = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '-' || b == '_' ||
                    b == '.' || b == '~';
        if (pos + (keep ? 1u : 3u) >= cap) {
            return 0;
        }
        if (keep) {
            out[pos++] = (char)b;
        } else {
            out[pos++] = '%';
            out[pos++] = hex[b >> 4];
            out[pos++] = hex[b & 15u];
        }
    }
    n = snprintf(out + pos, cap - pos, "&count=%d&language=%s&format=json", WX_GEOCODE_COUNT, lang && *lang ? lang : "en");
    return fit(n, cap - pos) ? pos + (size_t)n : 0;
}

size_t wx_url_ipinfo(char *out, size_t cap) { return fit(snprintf(out, cap, "https://ipinfo.io/json"), cap); }

size_t wx_url_forecast(char *out, size_t cap, const wx_place_t *place)
{
    char lat[16], lon[16];
    coord(lat, sizeof(lat), place->lat_e4);
    coord(lon, sizeof(lon), place->lon_e4);
    return fit(snprintf(out, cap,
                        "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
                        "&current=temperature_2m,weather_code,is_day"
                        "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
                        "&timezone=auto&forecast_days=1",
                        lat, lon),
               cap);
}

/* ---------------------------------------------------------------- scheduler */

static bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

void wx_init(wx_t *w, uint32_t now_ms)
{
    memset(w, 0, sizeof(*w));
    w->interval_ms = 60u * 60000u;
    memcpy(w->lang, "en", 3);
    w->next_ms = now_ms;
}

void wx_configure(wx_t *w, uint32_t now_ms, bool enabled, uint16_t interval_min, const uint8_t *city,
                  size_t city_len, const char *lang)
{
    city_len = city_len > WX_CITY_MAX ? WX_CITY_MAX : city_len;
    bool city_changed = city_len != w->city_len || (city_len && memcmp(city, w->city, city_len) != 0);
    bool lang_changed = lang && *lang && strncmp(lang, w->lang, sizeof(w->lang)) != 0;
    bool relocate = city_changed || (lang_changed && w->city_len && !w->located_by_ip);
    if (relocate) {
        w->located = false; /* geocode again: a new place, or its name in the new language */
        w->have = false;
        w->error = 0;
    }
    if (city_changed) {
        if (city_len) {
            memcpy(w->city, city, city_len);
        }
        w->city_len = city_len;
    }
    if (lang && *lang) {
        snprintf(w->lang, sizeof(w->lang), "%s", lang);
    }
    w->interval_ms = (uint32_t)(interval_min ? interval_min : 60u) * 60000u;
    if ((enabled && !w->enabled) || relocate) {
        w->next_ms = now_ms;
        w->fails = 0;
    }
    w->enabled = enabled;
}

void wx_kick(wx_t *w, uint32_t now_ms)
{
    w->next_ms = now_ms;
    w->fails = 0;
}

bool wx_due(const wx_t *w, uint32_t now_ms) { return w->enabled && !w->in_cycle && reached(now_ms, w->next_ms); }

void wx_begin(wx_t *w, uint32_t now_ms)
{
    w->cycle_ms = now_ms;
    w->in_cycle = true;
    w->pending = WX_REQ_NONE;
}

/* At the start of the cycle (located during this cycle counts as fresh). */
static bool location_valid(const wx_t *w)
{
    return w->located && (!w->located_by_ip || (int32_t)(w->cycle_ms - w->located_ms) < (int32_t)WX_IP_REFRESH_MS);
}

wx_req_t wx_next(wx_t *w, char *url, size_t cap)
{
    if (!w->in_cycle) {
        return WX_REQ_NONE;
    }
    wx_req_t r;
    size_t n;
    if (!location_valid(w)) {
        r = w->city_len ? WX_REQ_GEOCODE : WX_REQ_IPINFO;
        n = r == WX_REQ_GEOCODE ? wx_url_geocode(url, cap, w->city, w->city_len, w->lang) : wx_url_ipinfo(url, cap);
    } else {
        r = WX_REQ_FORECAST;
        n = wx_url_forecast(url, cap, &w->place);
    }
    if (n == 0) { /* cap too small: a programming error, end the cycle */
        w->in_cycle = false;
        return WX_REQ_NONE;
    }
    w->pending = r;
    return r;
}

static void fail(wx_t *w, uint32_t now_ms, uint8_t error, bool wait_full_interval)
{
    w->in_cycle = false;
    w->pending = WX_REQ_NONE;
    w->error = error;
    if (w->fails < 31) {
        w->fails++;
    }
    uint32_t delay = w->interval_ms;
    if (!wait_full_interval && w->fails <= 8) {
        uint32_t backoff = WX_RETRY_FIRST_MS << (w->fails - 1);
        delay = backoff < delay ? backoff : delay;
    }
    w->next_ms = now_ms + delay;
}

void wx_response(wx_t *w, uint32_t now_ms, int http_status, const char *body, size_t len)
{
    wx_req_t req = w->pending;
    if (!w->in_cycle || req == WX_REQ_NONE) {
        return;
    }
    w->pending = WX_REQ_NONE;
    const bool locating = req != WX_REQ_FORECAST;
    const uint8_t error = locating ? PROTO_WEATHER_STATE_LOCATE_FAILED : PROTO_WEATHER_STATE_FETCH_FAILED;
    if (http_status <= 0) {
        fail(w, now_ms, error, false);
        return;
    }
    if (locating) {
        wx_place_t pl;
        wx_result_t res = http_status != 200 ? WX_ERR_API
                        : req == WX_REQ_GEOCODE ? wx_parse_geocode(body, len, &pl)
                                                : wx_parse_ipinfo(body, len, &pl);
        if (res != WX_OK) {
            fail(w, now_ms, error, res == WX_ERR_NOT_FOUND); /* an unknown city stays unknown until it changes */
            return;
        }
        if (!pl.name[0] && req == WX_REQ_GEOCODE) { /* keep what the user typed */
            size_t n = w->city_len < WX_NAME_MAX ? w->city_len : WX_NAME_MAX;
            memcpy(pl.name, w->city, n);
            pl.name[n] = '\0';
        }
        w->place = pl;
        w->located = true;
        w->located_by_ip = req == WX_REQ_IPINFO;
        w->located_ms = now_ms;
        return; /* wx_next asks for the forecast */
    }
    wx_reading_t r;
    if (http_status != 200 || wx_parse_forecast(body, len, &r) != WX_OK) {
        fail(w, now_ms, error, false);
        return;
    }
    memcpy(r.place, w->place.name, sizeof(r.place));
    w->r = r;
    w->have = true;
    w->fetched_ms = now_ms;
    w->error = 0;
    w->fails = 0;
    w->in_cycle = false;
    w->next_ms = now_ms + w->interval_ms;
}

void wx_no_network(wx_t *w, uint32_t now_ms)
{
    w->in_cycle = false;
    w->pending = WX_REQ_NONE;
    if (w->fails < 31) {
        w->fails++;
    }
    uint32_t backoff = w->fails <= 8 ? WX_RETRY_FIRST_MS << (w->fails - 1) : w->interval_ms;
    w->next_ms = now_ms + (backoff < w->interval_ms ? backoff : w->interval_ms);
}

static bool fresh(const wx_t *w, uint32_t now_ms) { return w->have && now_ms - w->fetched_ms < WX_STALE_MS; }

uint8_t wx_state(const wx_t *w, uint32_t now_ms)
{
    if (!w->enabled) {
        return PROTO_WEATHER_STATE_OFF;
    }
    if (w->error) {
        return w->error;
    }
    if (!w->have) {
        return PROTO_WEATHER_STATE_WAITING;
    }
    return fresh(w, now_ms) ? PROTO_WEATHER_STATE_OK : PROTO_WEATHER_STATE_STALE;
}

const wx_reading_t *wx_current(const wx_t *w, uint32_t now_ms)
{
    return w->enabled && fresh(w, now_ms) ? &w->r : NULL;
}
