/* Weather (components/weather): JSON reader, parsers, URLs and the fetch scheduler. The forecast and
 * geocoding documents were recorded from Open-Meteo on 2026-10-14; the ipinfo one follows its
 * documented shape with a documentation address. */
#include <stdlib.h>
#include <string.h>

#include "epager/proto_gen.h"
#include "epager/wx.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const char FORECAST[] =
    "{\"latitude\":10.790861,\"longitude\":106.6313,\"generationtime_ms\":0.19788742065429688,"
    "\"utc_offset_seconds\":25200,\"timezone\":\"Asia/Ho_Chi_Minh\",\"timezone_abbreviation\":\"GMT+7\","
    "\"elevation\":7.0,\"current_units\":{\"time\":\"iso8601\",\"interval\":\"seconds\",\"temperature_2m\":"
    "\"\xc2\xb0" "C\",\"weather_code\":\"wmo code\",\"is_day\":\"\"},\"current\":{\"time\":\"2026-10-14T11:15\","
    "\"interval\":900,\"temperature_2m\":31.2,\"weather_code\":1,\"is_day\":1},\"daily_units\":{\"time\":"
    "\"iso8601\",\"temperature_2m_max\":\"\xc2\xb0" "C\",\"temperature_2m_min\":\"\xc2\xb0" "C\","
    "\"precipitation_probability_max\":\"%\"},\"daily\":{\"time\":[\"2026-10-14\"],\"temperature_2m_max\":[32.1],"
    "\"temperature_2m_min\":[24.6],\"precipitation_probability_max\":[86]}}";

static const char GEOCODE[] =
    "{\"results\":[{\"id\":1566083,\"name\":\"Ho Chi Minh City\",\"latitude\":10.82302,\"longitude\":106.62965,"
    "\"elevation\":7.0,\"feature_code\":\"PPL\",\"country_code\":\"VN\",\"admin1_id\":13494358,\"timezone\":"
    "\"Asia/Ho_Chi_Minh\",\"population\":14002598,\"country_id\":1562822,\"country\":\"Vietnam\",\"admin1\":"
    "\"Ho Chi Minh City (HCMC)\"}],\"generationtime_ms\":0.71942806}";

static const char GEOCODE_NONE[] = "{\"generationtime_ms\":0.4110336}";

/* "Hanoi" (abridged): the capital is not always first; a place without a population counts as smaller */
static const char GEOCODE_MANY[] =
    "{\"results\":[{\"name\":\"Hanoi\",\"latitude\":20.32773,\"longitude\":106.01208,\"feature_code\":\"PPLA2\"},"
    "{\"name\":\"H\u00e0 N\u1ed9i\",\"latitude\":21.0245,\"longitude\":105.84117,\"population\":8053663},"
    "{\"name\":\"B\u1ea1ch Mai\",\"latitude\":20.98333,\"longitude\":105.83333,\"population\":91308}],"
    "\"generationtime_ms\":0.5}";

static const char IPINFO[] =
    "{\n  \"ip\": \"203.0.113.7\",\n  \"city\": \"H\\u00e0 N\\u1ed9i\",\n  \"region\": \"Hanoi\",\n"
    "  \"country\": \"VN\",\n  \"loc\": \"21.0245,105.8412\",\n  \"timezone\": \"Asia/Bangkok\"\n}";

static const char IPINFO_BOGON[] = "{\"ip\":\"10.0.0.2\",\"bogon\":true}";

#define LEN(s) (sizeof(s) - 1)

/* ---------------------------------------------------------------- JSON reader */

static void test_json_get_paths(void)
{
    wx_span_t v;
    TEST_ASSERT_TRUE(wx_json_get(FORECAST, LEN(FORECAST), "current.temperature_2m", &v));
    TEST_ASSERT_EQUAL_STRING_LEN("31.2", v.p, v.n);
    TEST_ASSERT_EQUAL_UINT(4, v.n);
    TEST_ASSERT_TRUE(wx_json_get(FORECAST, LEN(FORECAST), "daily.temperature_2m_min[0]", &v));
    TEST_ASSERT_EQUAL_STRING_LEN("24.6", v.p, v.n);
    TEST_ASSERT_TRUE(wx_json_get(FORECAST, LEN(FORECAST), "current_units", &v));
    TEST_ASSERT_EQUAL_CHAR('{', v.p[0]);
    TEST_ASSERT_EQUAL_CHAR('}', v.p[v.n - 1]);
    TEST_ASSERT_TRUE(wx_json_get(GEOCODE, LEN(GEOCODE), "results[0].name", &v));
    TEST_ASSERT_EQUAL_STRING_LEN("\"Ho Chi Minh City\"", v.p, v.n);

    TEST_ASSERT_FALSE(wx_json_get(FORECAST, LEN(FORECAST), "daily.temperature_2m_min[1]", &v));
    TEST_ASSERT_FALSE(wx_json_get(FORECAST, LEN(FORECAST), "current.temperature", &v));  /* a prefix */
    TEST_ASSERT_FALSE(wx_json_get(FORECAST, LEN(FORECAST), "current.temperature_2m.x", &v));
    TEST_ASSERT_FALSE(wx_json_get(FORECAST, LEN(FORECAST), "current[0]", &v)); /* not an array */
    TEST_ASSERT_FALSE(wx_json_get(GEOCODE_NONE, LEN(GEOCODE_NONE), "results[0]", &v));

    const char spaced[] = " { \"a\" : [ 1 , { \"b\" : \"x\\\"y\" } ] , \"c\" : null } ";
    TEST_ASSERT_TRUE(wx_json_get(spaced, LEN(spaced), "a[1].b", &v));
    TEST_ASSERT_EQUAL_STRING_LEN("\"x\\\"y\"", v.p, v.n);
    TEST_ASSERT_TRUE(wx_json_get(spaced, LEN(spaced), "c", &v));
    TEST_ASSERT_EQUAL_STRING_LEN("null", v.p, v.n);
    TEST_ASSERT_TRUE(wx_json_get("[]", 2, "", &v));
    TEST_ASSERT_FALSE(wx_json_get("[]", 2, "[0]", &v));
    TEST_ASSERT_FALSE(wx_json_get("{}", 2, "a", &v));
}

static void test_json_malformed_and_deep(void)
{
    wx_span_t v;
    /* every prefix of a valid document fails cleanly or finds the value before the cut */
    for (size_t n = 0; n < LEN(FORECAST); n++) {
        bool ok = wx_json_get(FORECAST, n, "daily.precipitation_probability_max[0]", &v);
        TEST_ASSERT_TRUE(!ok || v.p + v.n <= FORECAST + n);
    }
    TEST_ASSERT_FALSE(wx_json_get("{\"a\" 1}", 7, "a", &v));
    TEST_ASSERT_FALSE(wx_json_get("{\"x\":1 \"a\":2}", 13, "a", &v));
    TEST_ASSERT_FALSE(wx_json_get("{\"x\":[1,2}, \"a\":2}", 18, "a", &v));
    TEST_ASSERT_FALSE(wx_json_get("{\"x\":\"open, \"a\":2}", 18, "a", &v)); /* "a" sits inside a broken string */

    char deep[200];
    size_t n = 0;
    n += (size_t)snprintf(deep + n, sizeof(deep) - n, "{\"x\":");
    for (int i = 0; i < WX_JSON_DEPTH + 2; i++) {
        deep[n++] = '[';
    }
    for (int i = 0; i < WX_JSON_DEPTH + 2; i++) {
        deep[n++] = ']';
    }
    n += (size_t)snprintf(deep + n, sizeof(deep) - n, ",\"a\":5}");
    TEST_ASSERT_FALSE(wx_json_get(deep, n, "a", &v)); /* too deep to skip */
}

static void test_json_fixed(void)
{
    struct { const char *s; unsigned d; bool ok; int32_t v; } t[] = {
        {"31.2", 1, true, 312}, {"24.6", 1, true, 246}, {"-0.05", 1, true, -1}, {"-0.04", 1, true, 0},
        {"86", 0, true, 86}, {"10.82302", 4, true, 108230}, {"106.62965", 4, true, 1066297}, {"0", 4, true, 0},
        {"-12.5", 0, true, -13}, {"7.0", 1, true, 70}, {"123456789", 0, true, 123456789},
        {"1234567890", 0, false, 0}, {"999999999.9", 1, false, 0}, {"1e5", 0, false, 0}, {"null", 1, false, 0},
        {"-", 0, false, 0}, {"1.", 1, false, 0}, {".5", 1, false, 0}, {"\"3\"", 0, false, 0}, {"", 0, false, 0},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        int32_t v = 12345;
        bool ok = wx_json_fixed((wx_span_t){t[i].s, strlen(t[i].s)}, t[i].d, &v);
        TEST_ASSERT_EQUAL_MESSAGE(t[i].ok, ok, t[i].s);
        if (ok) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(t[i].v, v, t[i].s);
        }
    }
}

static void test_json_string(void)
{
    char out[16];
    size_t n;
#define S(lit) ((wx_span_t){lit, LEN(lit)})
    TEST_ASSERT_TRUE(wx_json_string(S("\"H\\u00e0 N\\u1ed9i\""), out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_STRING("H\xc3\xa0 N\xe1\xbb\x99i", out);
    TEST_ASSERT_EQUAL_UINT(9, n);
    TEST_ASSERT_TRUE(wx_json_string(S("\"\\ud83d\\ude00\\n\\t\\/\\\"\""), out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_STRING("\xf0\x9f\x98\x80\n\t/\"", out);
    TEST_ASSERT_TRUE(wx_json_string(S("\"Hà Nội\""), out, sizeof(out), &n)); /* raw UTF-8 */
    TEST_ASSERT_EQUAL_STRING("Hà Nội", out);

    /* cut at a character boundary: "Hà Nội" is H(1) à(2) space(1) N(1) ộ(3) i(1) */
    TEST_ASSERT_TRUE(wx_json_string(S("\"Hà Nội\""), out, 8, &n)); /* 7 bytes fit: "Hà N" + not ộ */
    TEST_ASSERT_EQUAL_STRING("Hà N", out);
    TEST_ASSERT_TRUE(wx_json_string(S("\"H\\u00e0 N\\u1ed9i\""), out, 8, &n));
    TEST_ASSERT_EQUAL_STRING("Hà N", out);
    TEST_ASSERT_TRUE(wx_json_string(S("\"Hà\""), out, 3, &n));
    TEST_ASSERT_EQUAL_STRING("H", out);
    TEST_ASSERT_TRUE(wx_json_string(S("\"\""), out, 1, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);

    TEST_ASSERT_FALSE(wx_json_string(S("\"\\x\""), out, sizeof(out), &n));
    TEST_ASSERT_FALSE(wx_json_string(S("\"\\ud83d\""), out, sizeof(out), &n));        /* lone high */
    TEST_ASSERT_FALSE(wx_json_string(S("\"\\ude00\""), out, sizeof(out), &n));        /* lone low */
    TEST_ASSERT_FALSE(wx_json_string(S("\"\\ud83d\\u0041\""), out, sizeof(out), &n));  /* bad pair */
    TEST_ASSERT_FALSE(wx_json_string(S("\"a\\u0000b\""), out, sizeof(out), &n));
    TEST_ASSERT_FALSE(wx_json_string(S("\"\\u12\""), out, sizeof(out), &n));
    TEST_ASSERT_FALSE(wx_json_string(S("\"\xff\""), out, sizeof(out), &n));            /* invalid UTF-8 */
    TEST_ASSERT_FALSE(wx_json_string(S("\"\\\""), out, sizeof(out), &n));
    TEST_ASSERT_FALSE(wx_json_string(S("12"), out, sizeof(out), &n));
    TEST_ASSERT_FALSE(wx_json_string(S("\"x"), out, sizeof(out), &n));
#undef S
}

/* ---------------------------------------------------------------- model and parsers */

static void test_wmo_codes(void)
{
    struct { int32_t code; wx_cond_t c; } t[] = {
        {0, WX_COND_CLEAR}, {1, WX_COND_MOSTLY_CLEAR}, {2, WX_COND_PARTLY_CLOUDY}, {3, WX_COND_OVERCAST},
        {45, WX_COND_FOG}, {48, WX_COND_FOG}, {51, WX_COND_DRIZZLE}, {57, WX_COND_DRIZZLE}, {61, WX_COND_RAIN},
        {63, WX_COND_RAIN}, {65, WX_COND_HEAVY_RAIN}, {66, WX_COND_RAIN}, {67, WX_COND_HEAVY_RAIN},
        {71, WX_COND_SNOW}, {77, WX_COND_SNOW}, {80, WX_COND_SHOWERS}, {81, WX_COND_SHOWERS},
        {82, WX_COND_HEAVY_RAIN}, {85, WX_COND_SNOW}, {86, WX_COND_SNOW}, {95, WX_COND_THUNDER},
        {99, WX_COND_THUNDER}, {4, WX_COND_UNKNOWN}, {-1, WX_COND_UNKNOWN}, {100, WX_COND_UNKNOWN},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        TEST_ASSERT_EQUAL_INT(t[i].c, wx_cond_from_wmo(t[i].code));
    }
}

static void test_parse_forecast(void)
{
    wx_reading_t r;
    TEST_ASSERT_EQUAL_INT(WX_OK, wx_parse_forecast(FORECAST, LEN(FORECAST), &r));
    TEST_ASSERT_EQUAL_INT16(312, r.temp_c10);
    TEST_ASSERT_EQUAL_INT16(321, r.high_c10);
    TEST_ASSERT_EQUAL_INT16(246, r.low_c10);
    TEST_ASSERT_EQUAL_UINT8(86, r.precip_pct);
    TEST_ASSERT_EQUAL_UINT8(WX_COND_MOSTLY_CLEAR, r.cond);
    TEST_ASSERT_TRUE(r.is_day);
    TEST_ASSERT_TRUE(r.has_utc_offset);
    TEST_ASSERT_EQUAL_INT32(25200, r.utc_offset_s);

    const char minimal[] = "{\"current\":{\"temperature_2m\":-3.4,\"weather_code\":71,\"is_day\":0},"
                           "\"daily\":{\"precipitation_probability_max\":[null]},\"utc_offset_seconds\":99999}";
    TEST_ASSERT_EQUAL_INT(WX_OK, wx_parse_forecast(minimal, LEN(minimal), &r));
    TEST_ASSERT_EQUAL_INT16(-34, r.temp_c10);
    TEST_ASSERT_EQUAL_UINT8(WX_COND_SNOW, r.cond);
    TEST_ASSERT_FALSE(r.is_day);
    TEST_ASSERT_EQUAL_INT16(WX_TEMP_UNKNOWN, r.high_c10);
    TEST_ASSERT_EQUAL_INT16(WX_TEMP_UNKNOWN, r.low_c10);
    TEST_ASSERT_EQUAL_UINT8(WX_PCT_UNKNOWN, r.precip_pct);
    TEST_ASSERT_FALSE(r.has_utc_offset); /* out of range */

    const char err[] = "{\"error\":true,\"reason\":\"Latitude must be in range of -90 to 90\"}";
    TEST_ASSERT_EQUAL_INT(WX_ERR_API, wx_parse_forecast(err, LEN(err), &r));
    const char no_code[] = "{\"current\":{\"temperature_2m\":20}}";
    TEST_ASSERT_EQUAL_INT(WX_ERR_FORMAT, wx_parse_forecast(no_code, LEN(no_code), &r));
    const char hot[] = "{\"current\":{\"temperature_2m\":100.0,\"weather_code\":0}}";
    TEST_ASSERT_EQUAL_INT(WX_ERR_FORMAT, wx_parse_forecast(hot, LEN(hot), &r));
    TEST_ASSERT_EQUAL_INT(WX_ERR_FORMAT, wx_parse_forecast("<html>", 6, &r));
}

static void test_parse_places(void)
{
    wx_place_t p;
    TEST_ASSERT_EQUAL_INT(WX_OK, wx_parse_geocode(GEOCODE, LEN(GEOCODE), &p));
    TEST_ASSERT_EQUAL_INT32(108230, p.lat_e4);
    TEST_ASSERT_EQUAL_INT32(1066297, p.lon_e4);
    TEST_ASSERT_EQUAL_STRING("Ho Chi Minh City", p.name);
    TEST_ASSERT_EQUAL_INT(WX_ERR_NOT_FOUND, wx_parse_geocode(GEOCODE_NONE, LEN(GEOCODE_NONE), &p));
    TEST_ASSERT_EQUAL_INT(WX_OK, wx_parse_geocode(GEOCODE_MANY, LEN(GEOCODE_MANY), &p));
    TEST_ASSERT_EQUAL_INT32(210245, p.lat_e4);
    TEST_ASSERT_EQUAL_STRING("H\xc3\xa0 N\xe1\xbb\x99i", p.name);
    TEST_ASSERT_EQUAL_INT(WX_ERR_FORMAT, wx_parse_geocode("{}", 2, &p));
    const char far[] = "{\"results\":[{\"latitude\":95,\"longitude\":0}],\"generationtime_ms\":1}";
    TEST_ASSERT_EQUAL_INT(WX_ERR_FORMAT, wx_parse_geocode(far, LEN(far), &p));
    const char noname[] = "{\"results\":[{\"latitude\":-33.8688,\"longitude\":151.2093}],\"generationtime_ms\":1}";
    TEST_ASSERT_EQUAL_INT(WX_OK, wx_parse_geocode(noname, LEN(noname), &p));
    TEST_ASSERT_EQUAL_INT32(-338688, p.lat_e4);
    TEST_ASSERT_EQUAL_STRING("", p.name);

    TEST_ASSERT_EQUAL_INT(WX_OK, wx_parse_ipinfo(IPINFO, LEN(IPINFO), &p));
    TEST_ASSERT_EQUAL_INT32(210245, p.lat_e4);
    TEST_ASSERT_EQUAL_INT32(1058412, p.lon_e4);
    TEST_ASSERT_EQUAL_STRING("Hà Nội", p.name);
    TEST_ASSERT_EQUAL_INT(WX_ERR_NOT_FOUND, wx_parse_ipinfo(IPINFO_BOGON, LEN(IPINFO_BOGON), &p));
    const char bad_loc[] = "{\"ip\":\"1.2.3.4\",\"loc\":\"21.0\"}";
    TEST_ASSERT_EQUAL_INT(WX_ERR_FORMAT, wx_parse_ipinfo(bad_loc, LEN(bad_loc), &p));
    const char rate[] = "{\"error\":{\"title\":\"Rate limit exceeded\"}}";
    TEST_ASSERT_EQUAL_INT(WX_ERR_API, wx_parse_ipinfo(rate, LEN(rate), &p));
}

/* ---------------------------------------------------------------- URLs */

static void test_urls(void)
{
    char url[WX_URL_MAX];
    const char *city = "Hà Nội";
    size_t n = wx_url_geocode(url, sizeof(url), (const uint8_t *)city, strlen(city), "vi");
    TEST_ASSERT_EQUAL_STRING("https://geocoding-api.open-meteo.com/v1/search?name=H%C3%A0%20N%E1%BB%99i"
                             "&count=5&language=vi&format=json", url);
    TEST_ASSERT_EQUAL_UINT(strlen(url), n);
    TEST_ASSERT_EQUAL_UINT(0, wx_url_geocode(url, 60, (const uint8_t *)city, strlen(city), "vi"));
    uint8_t worst[WX_CITY_MAX];
    memset(worst, '&', sizeof(worst));
    TEST_ASSERT_NOT_EQUAL(0, wx_url_geocode(url, sizeof(url), worst, sizeof(worst), "en"));
    TEST_ASSERT_NULL(strchr(url + 50, ' '));

    TEST_ASSERT_EQUAL_UINT(22, wx_url_ipinfo(url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://ipinfo.io/json", url);

    wx_place_t p = {.lat_e4 = -5000, .lon_e4 = -1066297};
    TEST_ASSERT_NOT_EQUAL(0, wx_url_forecast(url, sizeof(url), &p));
    TEST_ASSERT_EQUAL_STRING("https://api.open-meteo.com/v1/forecast?latitude=-0.5000&longitude=-106.6297"
                             "&current=temperature_2m,weather_code,is_day"
                             "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
                             "&timezone=auto&forecast_days=1", url);
    TEST_ASSERT_EQUAL_UINT(0, wx_url_forecast(url, 40, &p));
}

/* ---------------------------------------------------------------- scheduler */

#define MIN_MS 60000u

static wx_t w;
static uint32_t now;
static char url[WX_URL_MAX];

/* One cycle: answers the requests in order with the given documents (NULL = no answer). */
static void cycle(const char *const *docs, const int *status, size_t n)
{
    TEST_ASSERT_TRUE(wx_due(&w, now));
    wx_begin(&w, now);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_EQUAL(WX_REQ_NONE, wx_next(&w, url, sizeof(url)));
        now += 700;
        wx_response(&w, now, status[i], docs[i], docs[i] ? strlen(docs[i]) : 0);
    }
    TEST_ASSERT_EQUAL(WX_REQ_NONE, wx_next(&w, url, sizeof(url)));
    TEST_ASSERT_FALSE(w.in_cycle);
}

static void ok_cycle_by_ip(void)
{
    const char *d[] = {IPINFO, FORECAST};
    const int s[] = {200, 200};
    cycle(d, s, 2);
}

static void test_sched_first_cycle_by_ip(void)
{
    now = 0xFFFFF000u; /* the millisecond clock wraps during the test */
    wx_init(&w, now);
    TEST_ASSERT_FALSE(wx_due(&w, now)); /* off until configured */
    TEST_ASSERT_EQUAL_UINT8(PROTO_WEATHER_STATE_OFF, wx_state(&w, now));
    wx_configure(&w, now, true, 30, NULL, 0, "en");
    TEST_ASSERT_EQUAL_UINT8(PROTO_WEATHER_STATE_WAITING, wx_state(&w, now));
    TEST_ASSERT_NULL(wx_current(&w, now));

    TEST_ASSERT_TRUE(wx_due(&w, now));
    wx_begin(&w, now);
    TEST_ASSERT_EQUAL(WX_REQ_IPINFO, wx_next(&w, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://ipinfo.io/json", url);
    wx_response(&w, now += 300, 200, IPINFO, LEN(IPINFO));
    TEST_ASSERT_EQUAL(WX_REQ_FORECAST, wx_next(&w, url, sizeof(url)));
    TEST_ASSERT_NOT_NULL(strstr(url, "latitude=21.0245&longitude=105.8412"));
    wx_response(&w, now += 300, 200, FORECAST, LEN(FORECAST));
    TEST_ASSERT_EQUAL(WX_REQ_NONE, wx_next(&w, url, sizeof(url)));

    const wx_reading_t *r = wx_current(&w, now);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_INT16(312, r->temp_c10);
    TEST_ASSERT_EQUAL_STRING("Hà Nội", r->place);
    TEST_ASSERT_EQUAL_UINT8(PROTO_WEATHER_STATE_OK, wx_state(&w, now));

    /* next fetch after the interval, without locating again */
    TEST_ASSERT_FALSE(wx_due(&w, now + 30 * MIN_MS - 1));
    now += 30 * MIN_MS;
    TEST_ASSERT_TRUE(wx_due(&w, now));
    wx_begin(&w, now);
    TEST_ASSERT_EQUAL(WX_REQ_FORECAST, wx_next(&w, url, sizeof(url)));
    wx_response(&w, now, 200, FORECAST, LEN(FORECAST));

    /* stale after 3 h without a new reading */
    now += WX_STALE_MS;
    TEST_ASSERT_NULL(wx_current(&w, now));
    TEST_ASSERT_EQUAL_UINT8(PROTO_WEATHER_STATE_STALE, wx_state(&w, now));

    /* the IP location is refreshed after 24 h */
    now += WX_IP_REFRESH_MS;
    ok_cycle_by_ip();
    TEST_ASSERT_EQUAL_UINT8(PROTO_WEATHER_STATE_OK, wx_state(&w, now));
}

static void test_sched_backoff(void)
{
    now = 1000;
    wx_init(&w, now);
    wx_configure(&w, now, true, 15, NULL, 0, "en");
    const uint32_t want_min[] = {1, 2, 4, 8, 15, 15};
    for (size_t i = 0; i < sizeof(want_min) / sizeof(want_min[0]); i++) {
        const char *d[] = {NULL};
        const int s[] = {-1};
        cycle(d, s, 1);
        TEST_ASSERT_EQUAL_UINT8(PROTO_WEATHER_STATE_LOCATE_FAILED, wx_state(&w, now));
        TEST_ASSERT_FALSE(wx_due(&w, now + want_min[i] * MIN_MS - 1));
        now += want_min[i] * MIN_MS;
    }
    /* located, but the forecast fails: fetch_failed, and the location is kept */
    const char *d1[] = {IPINFO, "{\"error\":true,\"reason\":\"x\"}"};
    const int s1[] = {200, 400};
    cycle(d1, s1, 2);
    TEST_ASSERT_EQUAL_UINT8(PROTO_WEATHER_STATE_FETCH_FAILED, wx_state(&w, now));
    TEST_ASSERT_FALSE(wx_due(&w, now + 15 * MIN_MS - 1)); /* the 7th failure in a row: the interval */
    now += 15 * MIN_MS;
    const char *d2[] = {FORECAST};
    const int s2[] = {200};
    cycle(d2, s2, 1);
    TEST_ASSERT_EQUAL_UINT8(PROTO_WEATHER_STATE_OK, wx_state(&w, now));
    TEST_ASSERT_EQUAL_UINT8(0, w.fails);

    /* no network: retried with backoff; the reading stays shown meanwhile */
    now += 15 * MIN_MS;
    TEST_ASSERT_TRUE(wx_due(&w, now));
    wx_no_network(&w, now);
    TEST_ASSERT_NOT_NULL(wx_current(&w, now));
    TEST_ASSERT_FALSE(wx_due(&w, now + MIN_MS - 1));
    TEST_ASSERT_TRUE(wx_due(&w, now + MIN_MS));
    wx_kick(&w, now);
    TEST_ASSERT_TRUE(wx_due(&w, now));
}

static void test_sched_city(void)
{
    now = 5000;
    wx_init(&w, now);
    const char *city = "Ho Chi Minh";
    wx_configure(&w, now, true, 30, (const uint8_t *)city, strlen(city), "en");
    const char *d[] = {GEOCODE, FORECAST};
    const int s[] = {200, 200};
    TEST_ASSERT_TRUE(wx_due(&w, now));
    wx_begin(&w, now);
    TEST_ASSERT_EQUAL(WX_REQ_GEOCODE, wx_next(&w, url, sizeof(url)));
    TEST_ASSERT_NOT_NULL(strstr(url, "name=Ho%20Chi%20Minh&count=5&language=en"));
    wx_response(&w, now, 200, GEOCODE, LEN(GEOCODE));
    TEST_ASSERT_EQUAL(WX_REQ_FORECAST, wx_next(&w, url, sizeof(url)));
    wx_response(&w, now, 200, FORECAST, LEN(FORECAST));
    TEST_ASSERT_EQUAL_STRING("Ho Chi Minh City", wx_current(&w, now)->place);

    /* the same settings again change nothing */
    wx_configure(&w, now, true, 30, (const uint8_t *)city, strlen(city), "en");
    TEST_ASSERT_FALSE(wx_due(&w, now));
    /* a city located by name is not refreshed after 24 h */
    now += WX_IP_REFRESH_MS + 1;
    wx_begin(&w, now);
    TEST_ASSERT_EQUAL(WX_REQ_FORECAST, wx_next(&w, url, sizeof(url)));
    wx_response(&w, now, 200, FORECAST, LEN(FORECAST));

    /* a new language looks the name up again */
    wx_configure(&w, now, true, 30, (const uint8_t *)city, strlen(city), "vi");
    TEST_ASSERT_NULL(wx_current(&w, now));
    cycle(d, s, 2);
    TEST_ASSERT_NOT_NULL(wx_current(&w, now));

    /* an unknown city: locate_failed, and no retry before the interval */
    const char *bad = "Xyzzy";
    wx_configure(&w, now, true, 30, (const uint8_t *)bad, strlen(bad), "vi");
    TEST_ASSERT_NULL(wx_current(&w, now));
    const char *d2[] = {GEOCODE_NONE};
    const int s2[] = {200};
    cycle(d2, s2, 1);
    TEST_ASSERT_EQUAL_UINT8(PROTO_WEATHER_STATE_LOCATE_FAILED, wx_state(&w, now));
    TEST_ASSERT_FALSE(wx_due(&w, now + 30 * MIN_MS - 1));

    /* clearing the city locates by IP */
    wx_configure(&w, now, true, 30, NULL, 0, "vi");
    TEST_ASSERT_TRUE(wx_due(&w, now));
    wx_begin(&w, now);
    TEST_ASSERT_EQUAL(WX_REQ_IPINFO, wx_next(&w, url, sizeof(url)));
    wx_response(&w, now, 503, "busy", 4);
    TEST_ASSERT_EQUAL_UINT8(PROTO_WEATHER_STATE_LOCATE_FAILED, wx_state(&w, now));

    /* switched off: no fetches, nothing shown */
    wx_configure(&w, now, false, 30, NULL, 0, "vi");
    TEST_ASSERT_FALSE(wx_due(&w, now + WX_STALE_MS));
    TEST_ASSERT_EQUAL_UINT8(PROTO_WEATHER_STATE_OFF, wx_state(&w, now));
}

static void test_sched_ignores_stray_responses(void)
{
    now = 1;
    wx_init(&w, now);
    wx_configure(&w, now, true, 30, NULL, 0, "en");
    wx_response(&w, now, 200, FORECAST, LEN(FORECAST)); /* no cycle */
    TEST_ASSERT_NULL(wx_current(&w, now));
    wx_begin(&w, now);
    wx_response(&w, now, 200, FORECAST, LEN(FORECAST)); /* nothing asked yet */
    TEST_ASSERT_NULL(wx_current(&w, now));
    TEST_ASSERT_EQUAL(WX_REQ_NONE, wx_next(&w, url, 10)); /* buffer too small ends the cycle */
    TEST_ASSERT_FALSE(w.in_cycle);
}

/* ---------------------------------------------------------------- fuzz */

static void test_fuzz_parsers(void)
{
    const char *docs[] = {FORECAST, GEOCODE, GEOCODE_NONE, IPINFO, IPINFO_BOGON};
    const char alphabet[] = "{}[]\":,\\-.0123456789eEtruefalsn ";
    uint32_t seed = 12345;
    char buf[1024];
    for (int it = 0; it < 20000; it++) {
        const char *src = docs[(unsigned)it % 5u];
        size_t n = strlen(src);
        memcpy(buf, src, n);
        for (int k = 0; k < 4 && n > 0; k++) {
            seed = seed * 1103515245u + 12345u;
            size_t pos = (seed >> 8) % n;
            seed = seed * 1103515245u + 12345u;
            unsigned op = (seed >> 16) % 3u;
            if (op == 0) {
                buf[pos] = alphabet[(seed >> 4) % (sizeof(alphabet) - 1)];
            } else if (op == 1) {
                buf[pos] = (char)(seed >> 3);
            } else {
                n = pos; /* truncate */
            }
        }
        wx_reading_t r;
        wx_place_t p;
        (void)wx_parse_forecast(buf, n, &r);
        (void)wx_parse_geocode(buf, n, &p);
        (void)wx_parse_ipinfo(buf, n, &p);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_json_get_paths);
    RUN_TEST(test_json_malformed_and_deep);
    RUN_TEST(test_json_fixed);
    RUN_TEST(test_json_string);
    RUN_TEST(test_wmo_codes);
    RUN_TEST(test_parse_forecast);
    RUN_TEST(test_parse_places);
    RUN_TEST(test_urls);
    RUN_TEST(test_sched_first_cycle_by_ip);
    RUN_TEST(test_sched_backoff);
    RUN_TEST(test_sched_city);
    RUN_TEST(test_sched_ignores_stray_responses);
    RUN_TEST(test_fuzz_parsers);
    return UNITY_END();
}
