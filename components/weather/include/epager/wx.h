/*
 * Weather (PLAN D60-D64): what the board fetches over Wi-Fi and how often. Portable C11, no ESP-IDF
 * or FreeRTOS headers, no allocation, no logging (a place name is personal data).
 *
 * - A bounded JSON reader: the value at a key path, numbers to fixed point, strings to UTF-8.
 * - Parsers for the three responses the board uses: Open-Meteo forecast and geocoding, ipinfo.io.
 * - URL builders for those services.
 * - The fetch scheduler: when to fetch, which request comes next (locate, then forecast), backoff,
 *   and the weather_state reported in DEVICE_STATUS. The platform glue (components/net) brings Wi-Fi
 *   up for a cycle, runs the requests wx_next() asks for, feeds the answers to wx_response() and
 *   switches Wi-Fi off when wx_next() returns WX_REQ_NONE.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- JSON reader */

#define WX_JSON_DEPTH 16 /* deeper documents are rejected */

typedef struct {
    const char *p; /* first byte of the value (a string's opening quote) */
    size_t n;
} wx_span_t;

/* The value at `path` in a JSON document: object keys separated by '.', array elements as "[i]"
 * ("current.temperature_2m", "daily.temperature_2m_max[0]", "results[0].name"). Keys are compared
 * byte for byte (no escapes in the keys this board asks for). False if absent or the document is
 * malformed up to the value. */
bool wx_json_get(const char *doc, size_t len, const char *path, wx_span_t *out);
/* A JSON number times 10^decimals, rounded half away from zero. False for anything else (null,
 * exponents, more than 9 integer digits, or a result outside int32). */
bool wx_json_fixed(wx_span_t v, unsigned decimals, int32_t *out);
/* A JSON string decoded to NUL-terminated UTF-8 in out[cap] (cap >= 1), cut at a character boundary
 * if longer. False if not a string, an invalid escape or invalid UTF-8. *len may be NULL. */
bool wx_json_string(wx_span_t v, char *out, size_t cap, size_t *len);

/* ---------------------------------------------------------------- model */

typedef enum {
    WX_COND_UNKNOWN = 0,
    WX_COND_CLEAR,
    WX_COND_MOSTLY_CLEAR,
    WX_COND_PARTLY_CLOUDY,
    WX_COND_OVERCAST,
    WX_COND_FOG,
    WX_COND_DRIZZLE,
    WX_COND_RAIN,
    WX_COND_HEAVY_RAIN,
    WX_COND_SHOWERS,
    WX_COND_SNOW,
    WX_COND_THUNDER,
    WX_COND_COUNT
} wx_cond_t;

#define WX_TEMP_UNKNOWN INT16_MIN
#define WX_PCT_UNKNOWN 255u
#define WX_NAME_MAX 48u /* bytes of a place name, without the NUL */
#define WX_CITY_MAX 64u /* weather.city max_len */

typedef struct {
    int32_t lat_e4, lon_e4;   /* degrees x 10^4 */
    char name[WX_NAME_MAX + 1];
} wx_place_t;

typedef struct {
    int16_t temp_c10;         /* current temperature, 0.1 deg C */
    int16_t high_c10, low_c10; /* today; WX_TEMP_UNKNOWN if missing */
    uint8_t precip_pct;       /* today's highest chance of rain; WX_PCT_UNKNOWN if missing */
    uint8_t cond;             /* wx_cond_t */
    bool is_day;
    bool has_utc_offset;
    int32_t utc_offset_s;     /* of the weather location (daylight saving included) */
    char place[WX_NAME_MAX + 1];
} wx_reading_t;

typedef enum {
    WX_OK = 0,
    WX_ERR_FORMAT,            /* not the expected document */
    WX_ERR_NOT_FOUND,         /* valid answer without a place (unknown city, private IP) */
    WX_ERR_API,               /* the service answered with an error object */
} wx_result_t;

/* WMO weather interpretation code (Open-Meteo `weather_code`) to a board condition. */
wx_cond_t wx_cond_from_wmo(int32_t code);

#define WX_GEOCODE_COUNT 5 /* places asked from the geocoder; the most populous wins (about 1.7 KB of JSON) */

wx_result_t wx_parse_forecast(const char *json, size_t len, wx_reading_t *out);
wx_result_t wx_parse_geocode(const char *json, size_t len, wx_place_t *out);
wx_result_t wx_parse_ipinfo(const char *json, size_t len, wx_place_t *out);

/* ---------------------------------------------------------------- URLs */

#define WX_URL_MAX 320u /* enough for the longest URL below with a 64-byte city fully escaped */

/* Each returns the length written to out[cap] (NUL-terminated), 0 if it does not fit. lang is an
 * ISO 639-1 code ("en", "vi"); the geocoder answers with names in that language where it has them. */
size_t wx_url_geocode(char *out, size_t cap, const uint8_t *city, size_t city_len, const char *lang);
size_t wx_url_ipinfo(char *out, size_t cap);
size_t wx_url_forecast(char *out, size_t cap, const wx_place_t *place);

/* ---------------------------------------------------------------- scheduler */

#define WX_STALE_MS (3u * 3600u * 1000u)        /* older readings are not shown */
#define WX_IP_REFRESH_MS (24u * 3600u * 1000u)  /* locate by IP again after this long */
#define WX_RETRY_FIRST_MS 60000u                /* backoff 1, 2, 4 ... minutes, up to the interval */

typedef enum { WX_REQ_NONE = 0, WX_REQ_GEOCODE, WX_REQ_IPINFO, WX_REQ_FORECAST } wx_req_t;

typedef struct {
    /* settings */
    bool enabled;
    uint32_t interval_ms;
    uint8_t city[WX_CITY_MAX];
    size_t city_len;
    char lang[3];
    /* location */
    bool located, located_by_ip;
    uint32_t located_ms;
    wx_place_t place;
    /* last reading */
    bool have;
    uint32_t fetched_ms;
    wx_reading_t r;
    /* schedule */
    bool in_cycle;
    uint32_t cycle_ms;
    wx_req_t pending;
    uint32_t next_ms;
    uint8_t fails;
    uint8_t error;            /* PROTO_WEATHER_STATE_LOCATE_FAILED / _FETCH_FAILED, or 0 */
} wx_t;

void wx_init(wx_t *w, uint32_t now_ms);
/* Settings from config (weather.enabled, weather.interval_min, weather.city, ui.language as "en"/"vi").
 * Switching on or changing the city makes a fetch due at once; a new city forgets the location. */
void wx_configure(wx_t *w, uint32_t now_ms, bool enabled, uint16_t interval_min, const uint8_t *city,
                  size_t city_len, const char *lang);
/* Make a fetch due now (e.g. a new Wi-Fi network was stored). */
void wx_kick(wx_t *w, uint32_t now_ms);
/* Time to start a fetch cycle. */
bool wx_due(const wx_t *w, uint32_t now_ms);
/* Start a cycle (Wi-Fi is up), then ask for requests until WX_REQ_NONE. */
void wx_begin(wx_t *w, uint32_t now_ms);
/* The next request of the cycle and its URL in url[cap] (cap >= WX_URL_MAX); WX_REQ_NONE: the
 * cycle is over. */
wx_req_t wx_next(wx_t *w, char *url, size_t cap);
/* The answer to the request wx_next returned: HTTP status (<= 0: no answer) and body. */
void wx_response(wx_t *w, uint32_t now_ms, int http_status, const char *body, size_t len);
/* The cycle could not start or was cut short (no network): retry with backoff. */
void wx_no_network(wx_t *w, uint32_t now_ms);

/* PROTO_WEATHER_STATE_* for DEVICE_STATUS. */
uint8_t wx_state(const wx_t *w, uint32_t now_ms);
/* The reading to show, NULL if there is none, it is stale or the weather is off. */
const wx_reading_t *wx_current(const wx_t *w, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
