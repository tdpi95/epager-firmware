/* Board core: dispatch, events and their order, read values, DEVICE_STATUS, clock, transfer and
 * OTA through the dispatcher. Dispatch vectors (dispatch.json) run in test_vectors. */
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "unity.h"
#include "xcore.h"

static xcore_t *x;

void setUp(void) { x = xcore_new(); }
void tearDown(void) { xcore_free(x); }

/* i-th reassembled message as compact JSON, with its characteristic. Static buffer. */
static const char *msg_at(int i, uint8_t *chr)
{
    static char out[4096];
    char *s = xcore_msg_json(x, i, chr);
    snprintf(out, sizeof(out), "%s", s);
    free(s);
    return out;
}

static int n_msgs(void)
{
    static xcore_msg_t m[32];
    return xcore_messages(x, m, 32);
}

static cJSON *field(const char *json, const char *name)
{
    cJSON *j = cJSON_Parse(json);
    cJSON *f = cJSON_DetachItemFromObject(cJSON_GetObjectItem(j, "fields"), name);
    cJSON_Delete(j);
    return f;
}

static double num_field(const char *json, const char *name)
{
    cJSON *f = field(json, name);
    double v = cJSON_IsNumber(f) ? f->valuedouble : -1;
    cJSON_Delete(f);
    return v;
}

#define ASSERT_CONTAINS(hay, needle) TEST_ASSERT_NOT_NULL_MESSAGE(strstr((hay), (needle)), (hay))

/* ---------------------------------------------------------------- read values */

static void test_capabilities_read(void)
{
    uint8_t b[PROTO_LIMIT_CAPABILITIES_MAX];
    size_t n = core_read(&x->core, CORE_CHR_CAPABILITIES, b, sizeof(b));
    TEST_ASSERT_GREATER_THAN(0, n);
    char *s = msg_to_json_str(b, n);
    ASSERT_CONTAINS(s, "\"type\":\"CAPABILITIES\"");
    ASSERT_CONTAINS(s, "\"contract_version\":\"" PROTO_CONTRACT_VERSION "\"");
    ASSERT_CONTAINS(s, "\"device_name\":\"ePager-1A2B\"");
    ASSERT_CONTAINS(s, "\"display\":{\"type\":1,\"width\":200,\"height\":200,\"color_depth\":1,\"refresh_class\":1}");
    ASSERT_CONTAINS(s, "\"storage_total\":16384,\"storage_free\":16384");
    ASSERT_CONTAINS(s, "\"asset_support\":[{\"asset_type\":3,\"format\":4,\"max_size\":512},{\"asset_type\":2");
    ASSERT_CONTAINS(s, "\"max_mtu\":517,\"config_revision\":0,\"pairing_mode\":false");
    ASSERT_CONTAINS(s, "\"builtin_face\":[\"epager.big\",\"epager.minimal\"]"); /* since 0.16.0 */
    free(s);
    /* readable without a secure link; Status is not */
    core_set_secure(&x->core, false);
    TEST_ASSERT_EQUAL_UINT(n, core_read(&x->core, CORE_CHR_CAPABILITIES, b, sizeof(b)));
    TEST_ASSERT_EQUAL_UINT(0, core_read(&x->core, CORE_CHR_STATUS, b, sizeof(b)));
    TEST_ASSERT_EQUAL_UINT(0, core_read(&x->core, CORE_CHR_CONFIG, b, sizeof(b)));
    /* so pairing mode is visible before bonding (since 0.7.0) */
    core_set_pairing_mode(&x->core, true);
    n = core_read(&x->core, CORE_CHR_CAPABILITIES, b, sizeof(b));
    s = msg_to_json_str(b, n);
    ASSERT_CONTAINS(s, "\"config_revision\":0,\"pairing_mode\":true");
    free(s);
}

static void test_status_read(void)
{
    uint8_t b[64];
    size_t n = core_read(&x->core, CORE_CHR_STATUS, b, sizeof(b));
    char *s = msg_to_json_str(b, n);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"DEVICE_STATUS\",\"req_id\":0,\"fields\":{\"battery_pct\":80,"
                             "\"external_power\":0,\"uptime_s\":1,\"notif_count\":0,\"bonded_count\":0,"
                             "\"pairing_mode\":false,\"ota_state\":0}}", s);
    free(s);
}

static void test_battery_unknown(void)
{
    x->battery_ok = false;
    core_poll_battery(&x->core);
    TEST_ASSERT_EQUAL_UINT8(CORE_BATTERY_UNKNOWN, core_battery_pct(&x->core));
    x->battery_ok = true;
    x->battery_pct = 150; /* a broken reader */
    core_poll_battery(&x->core);
    TEST_ASSERT_EQUAL_UINT8(CORE_BATTERY_UNKNOWN, core_battery_pct(&x->core));
    x->has_battery = false;
    xcore_reboot(x, false, true);
    TEST_ASSERT_EQUAL_UINT8(CORE_BATTERY_UNKNOWN, core_battery_pct(&x->core));
    uint8_t b[64];
    char *s = msg_to_json_str(b, core_read(&x->core, CORE_CHR_STATUS, b, sizeof(b)));
    ASSERT_CONTAINS(s, "\"battery_pct\":255,\"external_power\":2");
    free(s);
}

/* ---------------------------------------------------------------- DEVICE_STATUS events */

static void test_device_status_on_change(void)
{
    x->n_out = 0;
    x->battery_pct = 55;
    x->now_ms += 5000;
    core_tick(&x->core); /* poll interval not reached */
    TEST_ASSERT_EQUAL_INT(0, x->n_out);
    x->now_ms += 5000;
    core_tick(&x->core);
    uint8_t chr = 0;
    ASSERT_CONTAINS(msg_at(0, &chr), "\"battery_pct\":55");
    TEST_ASSERT_EQUAL_UINT8(CORE_CHR_STATUS, chr);
    x->n_out = 0;
    x->now_ms += 10000;
    core_tick(&x->core); /* same level: nothing */
    TEST_ASSERT_EQUAL_INT(0, x->n_out);
    x->external_power = PROTO_EXTERNAL_POWER_YES;
    core_poll_battery(&x->core);
    ASSERT_CONTAINS(msg_at(0, NULL), "\"external_power\":1");
    x->n_out = 0;
    core_set_pairing_mode(&x->core, true);
    ASSERT_CONTAINS(msg_at(0, NULL), "\"pairing_mode\":true");
    x->n_out = 0;
    core_set_bonded_count(&x->core, 2);
    ASSERT_CONTAINS(msg_at(0, NULL), "\"bonded_count\":2");
    x->n_out = 0;
    core_set_bonded_count(&x->core, 2);
    TEST_ASSERT_EQUAL_INT(0, x->n_out);
}

static void test_wifi_and_weather_status(void)
{
    /* without the features the fields are not sent and the setters only keep the values */
    core_set_wifi_status(&x->core, PROTO_WIFI_STATE_CONNECTED, "Home");
    uint8_t b[128];
    char *s = msg_to_json_str(b, core_read(&x->core, CORE_CHR_STATUS, b, sizeof(b)));
    TEST_ASSERT_NULL(strstr(s, "wifi"));
    TEST_ASSERT_NULL(strstr(s, "weather"));
    free(s);

    x->has_wifi = true;
    xcore_reboot(x, false, true);
    s = msg_to_json_str(b, core_read(&x->core, CORE_CHR_STATUS, b, sizeof(b)));
    ASSERT_CONTAINS(s, "\"ota_state\":0,\"wifi_state\":0,\"weather_state\":0}"); /* no ssid stored */
    free(s);
    x->n_out = 0;
    core_set_wifi_status(&x->core, PROTO_WIFI_STATE_CONNECTING, "Quán Cafe 5G");
    ASSERT_CONTAINS(msg_at(0, NULL), "\"wifi_state\":2,\"wifi_ssid\":\"Quán Cafe 5G\"");
    x->n_out = 0;
    core_set_wifi_status(&x->core, PROTO_WIFI_STATE_CONNECTING, "Quán Cafe 5G"); /* unchanged */
    TEST_ASSERT_EQUAL_INT(0, x->n_out);
    core_set_weather_state(&x->core, PROTO_WEATHER_STATE_OK);
    ASSERT_CONTAINS(msg_at(0, NULL), "\"weather_state\":2");
    x->n_out = 0;
    core_set_wifi_status(&x->core, PROTO_WIFI_STATE_IDLE, "0123456789abcdef0123456789abcdefXYZ"); /* cut at 32 */
    ASSERT_CONTAINS(msg_at(0, NULL), "\"wifi_ssid\":\"0123456789abcdef0123456789abcdef\"");
}

static void test_wifi_set_platform_error_and_events(void)
{
    x->has_wifi = true;
    xcore_reboot(x, false, true);
    x->wifi_result = PROTO_INTERNAL;
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"WIFI_SET\",\"req_id\":3,\"fields\":{\"ssid\":\"Home\"}}");
    TEST_ASSERT_EQUAL_INT(1, n_msgs()); /* nothing stored: no DEVICE_STATUS */
    ASSERT_CONTAINS(msg_at(0, NULL), "\"type\":\"WIFI_SET_RSP\",\"req_id\":3,\"fields\":{\"status\":27}");
    x->wifi_result = PROTO_OK;
    xcore_send_json(x, CORE_CHR_CONFIG,
                    "{\"type\":\"WIFI_SET\",\"req_id\":4,\"fields\":{\"ssid\":\"Home\",\"password\":\"12345678\"}}");
    TEST_ASSERT_EQUAL_INT(2, n_msgs()); /* the response, then the status event */
    uint8_t chr = 0;
    ASSERT_CONTAINS(msg_at(0, &chr), "WIFI_SET_RSP");
    TEST_ASSERT_EQUAL_UINT8(CORE_CHR_CONFIG, chr);
    ASSERT_CONTAINS(msg_at(1, &chr), "\"wifi_state\":1,\"wifi_ssid\":\"Home\"");
    TEST_ASSERT_EQUAL_UINT8(CORE_CHR_STATUS, chr);
    TEST_ASSERT_EQUAL_STRING("12345678", x->wifi_pw);
    /* WIFI_SET belongs to the Config characteristic */
    xcore_send_json(x, CORE_CHR_NOTIFICATION, "{\"type\":\"WIFI_SET\",\"req_id\":5,\"fields\":{\"forget\":true}}");
    ASSERT_CONTAINS(msg_at(0, NULL), "\"type\":\"ERROR\"");
    TEST_ASSERT_EQUAL_STRING("Home", x->wifi_ssid);
}

static void test_no_device_status_for_notif_count(void)
{
    xcore_send_json(x, CORE_CHR_NOTIFICATION, "{\"type\":\"NOTIF_POST\",\"req_id\":1,\"fields\":{\"key\":\"k1\","
                                              "\"app_id\":\"com.zing.zalo\",\"title\":\"Xin chào\"}}");
    TEST_ASSERT_EQUAL_INT(2, n_msgs()); /* response + APP_SEEN, no DEVICE_STATUS */
    uint8_t b[64];
    char *s = msg_to_json_str(b, core_read(&x->core, CORE_CHR_STATUS, b, sizeof(b)));
    ASSERT_CONTAINS(s, "\"notif_count\":1");
    free(s);
}

static void test_disconnected_sends_nothing(void)
{
    core_disconnect(&x->core);
    core_disconnect(&x->core); /* twice is harmless */
    x->n_out = 0;
    core_set_pairing_mode(&x->core, true);
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"PING\",\"req_id\":1,\"fields\":{}}");
    TEST_ASSERT_EQUAL_INT(0, x->n_out);
    /* the change is not replayed on the next connection: the app reads Status */
    core_connect(&x->core, 185, true);
    core_tick(&x->core);
    TEST_ASSERT_EQUAL_INT(0, x->n_out);
}

/* The platform is told when the phone changed a setting, and only then (PLAN D79). */
static void test_on_setting_only_for_the_phone(void)
{
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"CONFIG_SET\",\"req_id\":7,\"fields\":{\"entry\":["
                                        "{\"key\":4,\"value\":\"00\"},{\"key\":3,\"value\":\"05\"}]}}");
    TEST_ASSERT_EQUAL_INT(1, x->n_setting[CORE_SETTING_CONFIG]); /* once per request, not per key */
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"CONFIG_SET\",\"req_id\":8,\"fields\":{\"entry\":["
                                        "{\"key\":4,\"value\":\"00\"}]}}"); /* nothing changed */
    TEST_ASSERT_EQUAL_INT(1, x->n_setting[CORE_SETTING_CONFIG]);
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"CONFIG_SET\",\"req_id\":9,\"fields\":{\"entry\":["
                                        "{\"key\":4,\"value\":\"05\"}]}}"); /* wrong size: refused */
    TEST_ASSERT_EQUAL_INT(1, x->n_setting[CORE_SETTING_CONFIG]);
    /* a time zone that came with TIME_SYNC is not a setting the user changed */
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"TIME_SYNC\",\"req_id\":10,\"fields\":{\"utc_s\":1791700000,\"tz_offset_min\":420}}");
    TEST_ASSERT_EQUAL_INT(1, x->n_setting[CORE_SETTING_CONFIG]);
    /* the board's own write (the weather's time zone, the brightness keys) is not the phone's */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_config_set_u8(&x->core, PROTO_CFG_DISPLAY_FULL_REFRESH_EVERY, 7));
    TEST_ASSERT_EQUAL_INT(3, x->n_config_events);
    TEST_ASSERT_EQUAL_INT(1, x->n_setting[CORE_SETTING_CONFIG]);
    /* a network from the phone */
    x->has_wifi = true;
    xcore_reboot(x, false, true);
    x->wifi_result = PROTO_INTERNAL;
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"WIFI_SET\",\"req_id\":3,\"fields\":{\"ssid\":\"Home\"}}");
    TEST_ASSERT_EQUAL_INT(0, x->n_setting[CORE_SETTING_WIFI]); /* not stored */
    x->wifi_result = PROTO_OK;
    xcore_send_json(x, CORE_CHR_CONFIG,
                    "{\"type\":\"WIFI_SET\",\"req_id\":4,\"fields\":{\"ssid\":\"Home\",\"password\":\"12345678\"}}");
    TEST_ASSERT_EQUAL_INT(1, x->n_setting[CORE_SETTING_WIFI]);
}

/* ---------------------------------------------------------------- events after the response */

static void test_config_changed_after_response(void)
{
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"CONFIG_SET\",\"req_id\":7,\"fields\":{\"entry\":["
                                        "{\"key\":4,\"value\":\"00\"},{\"key\":3,\"value\":\"05\"}]}}");
    uint8_t c0 = 0, c1 = 0;
    ASSERT_CONTAINS(msg_at(0, &c0), "\"type\":\"CONFIG_SET_RSP\",\"req_id\":7,\"fields\":{\"status\":0,\"revision\":1");
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"CONFIG_CHANGED\",\"req_id\":0,\"fields\":{\"revision\":1,\"key\":[3,4]}}",
                             msg_at(1, &c1));
    TEST_ASSERT_EQUAL_UINT8(CORE_CHR_CONFIG, c0);
    TEST_ASSERT_EQUAL_UINT8(CORE_CHR_CONFIG, c1);
    TEST_ASSERT_EQUAL_INT(1, x->n_config_events);
    TEST_ASSERT_EQUAL_INT32(5, cfg_get_int(&x->core.cfg, PROTO_CFG_DISPLAY_FULL_REFRESH_EVERY));
    /* the same values again: no revision bump, no event */
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"CONFIG_SET\",\"req_id\":8,\"fields\":{\"entry\":["
                                        "{\"key\":4,\"value\":\"00\"}]}}");
    TEST_ASSERT_EQUAL_INT(1, n_msgs());
    /* a policy change bumps the revision with an empty CONFIG_CHANGED */
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"APPS_SET_POLICY\",\"req_id\":9,\"fields\":{\"entry\":["
                                        "{\"app_id\":\"com.x\",\"policy\":2}]}}");
    ASSERT_CONTAINS(msg_at(0, NULL), "APPS_SET_POLICY_RSP");
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"CONFIG_CHANGED\",\"req_id\":0,\"fields\":{\"revision\":2}}", msg_at(1, NULL));
}

static void test_app_seen_after_response(void)
{
    const char *post = "{\"type\":\"NOTIF_POST\",\"req_id\":3,\"fields\":{\"key\":\"k1\",\"app_id\":\"com.zing.zalo\","
                       "\"app_name\":\"Zalo\",\"title\":\"Tin nhắn mới\",\"body\":\"Chào bạn 👋\"}}";
    xcore_send_json(x, CORE_CHR_NOTIFICATION, post);
    uint8_t c0 = 0, c1 = 0;
    ASSERT_CONTAINS(msg_at(0, &c0), "\"type\":\"NOTIF_POST_RSP\",\"req_id\":3,\"fields\":{\"status\":0,\"notif_id\":1,\"shown\":true}");
    ASSERT_CONTAINS(msg_at(1, &c1), "\"type\":\"APP_SEEN\"");
    TEST_ASSERT_EQUAL_UINT8(CORE_CHR_NOTIFICATION, c0);
    TEST_ASSERT_EQUAL_UINT8(CORE_CHR_CONFIG, c1);
    TEST_ASSERT_EQUAL_INT(1, x->n_notif_events);
    TEST_ASSERT_EQUAL_STRING("Chào bạn 👋", x->last_notif.body); /* normalised for display; emoji are in the font */
    xcore_send_json(x, CORE_CHR_NOTIFICATION, post);
    TEST_ASSERT_EQUAL_INT(1, n_msgs()); /* known app: no APP_SEEN */
    TEST_ASSERT_EQUAL_UINT8(NPIPE_MODIFIED, x->last_notif.kind);
}

static void test_ancs_app_seen_outside_request(void)
{
    const char *id = "com.apple.MobileSMS";
    npipe_ancs_t in = {.ns = {.event_id = 0, .category_id = PROTO_CATEGORY_SOCIAL, .uid = 7},
                       .app_id = {(const uint8_t *)id, strlen(id)},
                       .title = {(const uint8_t *)"Mẹ", strlen("Mẹ")}};
    x->n_out = 0;
    npipe_ancs_event(&x->core.np, &in);
    uint8_t chr = 0;
    ASSERT_CONTAINS(msg_at(0, &chr), "\"type\":\"APP_SEEN\"");
    TEST_ASSERT_EQUAL_UINT8(CORE_CHR_CONFIG, chr);
    TEST_ASSERT_EQUAL_STRING("Mẹ", x->last_notif.title);
}

static void test_dismiss_event(void)
{
    const char *post = "{\"type\":\"NOTIF_POST\",\"req_id\":4,\"fields\":{\"key\":\"0|com.zing.zalo|1001\","
                       "\"app_id\":\"com.zing.zalo\",\"title\":\"Hi\"}}";
    xcore_send_json(x, CORE_CHR_NOTIFICATION, post);
    x->n_out = 0;
    TEST_ASSERT_FALSE(core_dismiss(&x->core, 99));
    TEST_ASSERT_EQUAL_INT(0, n_msgs());
    TEST_ASSERT_TRUE(core_dismiss(&x->core, 1));
    uint8_t chr = 0;
    ASSERT_CONTAINS(msg_at(0, &chr), "\"type\":\"NOTIF_DISMISSED\",\"req_id\":0,\"fields\":{\"notif_id\":1,"
                                     "\"source\":2,\"key\":\"0|com.zing.zalo|1001\"}");
    TEST_ASSERT_EQUAL_UINT8(CORE_CHR_NOTIFICATION, chr);
    TEST_ASSERT_EQUAL_UINT8(NPIPE_REMOVED, x->last_notif.kind);
    TEST_ASSERT_FALSE(core_dismiss(&x->core, 1));
    TEST_ASSERT_EQUAL_INT(1, x->n_dismissed); /* only the dismissal that removed something */
    TEST_ASSERT_EQUAL_UINT8(PROTO_NOTIF_SOURCE_ANDROID, x->last_dismissed.source);
}

/* An iPhone notification: the platform learns the ANCS UID and whether iOS offers the negative action. */
static void test_dismiss_reports_the_ancs_uid(void)
{
    npipe_ancs_t in = {.ns = {PROTO_ANCS_EVENT_ADDED, PROTO_ANCS_FLAG_NEGATIVE_ACTION, PROTO_CATEGORY_SOCIAL, 1, 0xCAFE},
                       .app_id = {(const uint8_t *)"com.apple.MobileSMS", 19}};
    npipe_ancs_event(&x->core.np, &in);
    const uint32_t id = x->last_notif.id;
    TEST_ASSERT_TRUE(core_dismiss(&x->core, id));
    TEST_ASSERT_EQUAL_INT(1, x->n_dismissed);
    TEST_ASSERT_EQUAL_UINT8(PROTO_NOTIF_SOURCE_ANCS, x->last_dismissed.source);
    TEST_ASSERT_EQUAL_UINT32(0xCAFE, x->last_dismissed.ancs_uid);
    TEST_ASSERT_TRUE(x->last_dismissed.flags & PROTO_NOTIF_FLAGS_NEGATIVE_ACTION);
}

static void test_config_set_on_the_board(void)
{
    x->n_out = 0;
    /* brightness exists on LCD only: the Paper test core answers UNKNOWN_KEY per key, status OK */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_config_set_u8(&x->core, PROTO_CFG_UI_LANGUAGE, PROTO_LANGUAGE_VI));
    uint8_t chr = 0;
    ASSERT_CONTAINS(msg_at(0, &chr), "\"type\":\"CONFIG_CHANGED\"");
    TEST_ASSERT_EQUAL_UINT8(CORE_CHR_CONFIG, chr);
    TEST_ASSERT_EQUAL_INT(PROTO_LANGUAGE_VI, cfg_get_int(&x->core.cfg, PROTO_CFG_UI_LANGUAGE));
    TEST_ASSERT_EQUAL_INT(PROTO_OUT_OF_RANGE, core_config_set_u8(&x->core, PROTO_CFG_UI_LANGUAGE, 9));
}

/* ---------------------------------------------------------------- clock */

static void test_config_set_any_type_on_the_board(void)
{
    x->n_out = 0;
    const uint8_t tz[2] = {0xA4, 0x01}; /* 420, little-endian */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_config_set(&x->core, PROTO_CFG_TIME_TZ_OFFSET_MIN, tz, 2));
    ASSERT_CONTAINS(msg_at(0, NULL), "\"type\":\"CONFIG_CHANGED\"");
    TEST_ASSERT_EQUAL_INT32(420, cfg_get_int(&x->core.cfg, PROTO_CFG_TIME_TZ_OFFSET_MIN));
    TEST_ASSERT_EQUAL_INT(PROTO_UNKNOWN_KEY, core_config_set(&x->core, PROTO_CFG_DISPLAY_BRIGHTNESS, (const uint8_t *)"\x46", 1));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_config_set(&x->core, PROTO_CFG_WEATHER_CITY, (const uint8_t *)"Hue", 3)); /* 0.9.1 */
    TEST_ASSERT_EQUAL_INT(PROTO_TYPE_MISMATCH, core_config_set(&x->core, PROTO_CFG_TIME_TZ_OFFSET_MIN, tz, 1));
}

static void test_time_sync_age(void)
{
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, core_time_sync_age_ms(&x->core));
    core_set_utc(&x->core, 1791700000); /* the board's own source: not a phone */
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, core_time_sync_age_ms(&x->core));
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"TIME_SYNC\",\"req_id\":1,\"fields\":{\"utc_s\":1791700215,"
                                        "\"tz_offset_min\":420}}");
    TEST_ASSERT_EQUAL_UINT32(0, core_time_sync_age_ms(&x->core));
    x->now_ms += 5000;
    TEST_ASSERT_EQUAL_UINT32(5000, core_time_sync_age_ms(&x->core));
}

static void test_time_sync_sets_clock(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, core_now_utc(&x->core));
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"TIME_SYNC\",\"req_id\":2,\"fields\":{\"utc_s\":1791700215,"
                                        "\"tz_offset_min\":420}}");
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"TIME_SYNC_RSP\",\"req_id\":2,\"fields\":{\"status\":0}}", msg_at(0, NULL));
    ASSERT_CONTAINS(msg_at(1, NULL), "\"type\":\"CONFIG_CHANGED\""); /* tz offset stored as config */
    TEST_ASSERT_EQUAL_UINT32(1791700215, core_now_utc(&x->core));
    x->now_ms += 5500;
    TEST_ASSERT_EQUAL_UINT32(1791700220, core_now_utc(&x->core));
    x->now_ms += 2u * 3600000u; /* rebase keeps sub-second precision */
    core_tick(&x->core);
    x->now_ms += 500;
    TEST_ASSERT_EQUAL_UINT32(1791700215 + 7206, core_now_utc(&x->core));
}

static void test_clock_across_ms_wrap(void)
{
    x->now_ms = 0xFFFFF000u;
    core_set_utc(&x->core, 1000);
    x->now_ms += 0x2000; /* wraps */
    TEST_ASSERT_EQUAL_UINT32(1008, core_now_utc(&x->core));
    core_set_utc(&x->core, 0);
    TEST_ASSERT_EQUAL_UINT32(0, core_now_utc(&x->core));
}

static void test_rtc_sets_clock_at_boot_and_time_sync_writes_it(void)
{
    x->has_rtc = true;
    xcore_reboot(x, false, true); /* RTC present but holding no time */
    TEST_ASSERT_EQUAL_UINT32(0, core_now_utc(&x->core));
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"TIME_SYNC\",\"req_id\":3,\"fields\":{\"utc_s\":1791700215,"
                                        "\"tz_offset_min\":420}}");
    TEST_ASSERT_EQUAL_INT(1, x->rtc_writes);
    TEST_ASSERT_EQUAL_UINT32(1791700215, x->rtc_utc);
    x->now_ms += 60000;
    xcore_reboot(x, false, true); /* RAM lost: the clock comes back from the RTC */
    TEST_ASSERT_EQUAL_UINT32(1791700275, core_now_utc(&x->core));
    TEST_ASSERT_EQUAL_INT(1, x->rtc_writes); /* reading it back does not write it */
    /* utc 0 clears the soft clock but is never written to the RTC */
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"TIME_SYNC\",\"req_id\":4,\"fields\":{\"utc_s\":0,"
                                        "\"tz_offset_min\":420}}");
    TEST_ASSERT_EQUAL_UINT32(0, core_now_utc(&x->core));
    TEST_ASSERT_EQUAL_INT(1, x->rtc_writes);
}

static void test_no_rtc_clock_lost_at_reboot(void)
{
    core_set_utc(&x->core, 1791700215);
    xcore_reboot(x, false, true);
    TEST_ASSERT_EQUAL_UINT32(0, core_now_utc(&x->core));
}

/* ---------------------------------------------------------------- dispatch details */

static void test_read_only_and_unknown_characteristics(void)
{
    uint8_t ping[] = {0xC0, 0, PROTO_MSG_PING, 1, 0};
    x->n_out = 0;
    core_write(&x->core, CORE_CHR_CAPABILITIES, ping, sizeof(ping));
    core_write(&x->core, CORE_CHR_STATUS, ping, sizeof(ping));
    core_write(&x->core, 9, ping, sizeof(ping));
    TEST_ASSERT_EQUAL_INT(0, x->n_out);
    core_write(&x->core, CORE_CHR_TRANSFER, ping, sizeof(ping));
    TEST_ASSERT_EQUAL_INT(1, x->n_out);
}

static void test_mtu_and_max_chunk(void)
{
    TEST_ASSERT_EQUAL_UINT16(1, core_max_chunk(23));
    TEST_ASSERT_EQUAL_UINT16(163, core_max_chunk(185));
    TEST_ASSERT_EQUAL_UINT16(493, core_max_chunk(517));
    TEST_ASSERT_EQUAL_UINT16(493, core_max_chunk(600));
    TEST_ASSERT_EQUAL_UINT(512, core_segment_max(517));
    TEST_ASSERT_EQUAL_UINT(20, core_segment_max(23));
    TEST_ASSERT_EQUAL_UINT16(1, core_max_chunk(5));
    core_set_mtu(&x->core, 10);
    TEST_ASSERT_EQUAL_UINT16(CORE_MTU_DEFAULT, x->core.mtu);
    core_set_mtu(&x->core, 9000);
    TEST_ASSERT_EQUAL_UINT16(CORE_MTU_MAX, x->core.mtu);
    TEST_ASSERT_EQUAL_UINT16(493, x->core.xfer.p.max_chunk);
    /* a response is segmented at the MTU: 300-byte PING payload at MTU 23 */
    core_set_mtu(&x->core, 23);
    char json[1024] = "{\"type\":\"PING\",\"req_id\":4,\"fields\":{\"payload\":\"";
    for (int i = 0; i < 300; i++) {
        strcat(json, "ab");
    }
    strcat(json, "\"}}");
    xcore_send_json(x, CORE_CHR_NOTIFICATION, json);
    TEST_ASSERT_EQUAL_INT((4 + 3 + 1 + 3 + 300 + 18) / 19, x->n_out);
    for (int i = 0; i < x->n_out; i++) {
        TEST_ASSERT_LESS_OR_EQUAL(20, x->out[i].len);
    }
    TEST_ASSERT_EQUAL_INT(1, n_msgs());
}

/* ---------------------------------------------------------------- transfer through the core */

static void hexs(const uint8_t *d, size_t n, char *out) { hex_encode(d, n, out); }

static size_t theme_asset(uint8_t *f, size_t payload)
{
    memcpy(f, "ESPA", 4);
    f[4] = 1;
    f[5] = PROTO_ASSET_TYPE_THEME;
    f[6] = PROTO_ASSET_FORMAT_THEME_V1;
    f[7] = 0;
    memset(f + 8, 0, 4);
    proto_wr32(f + 12, (uint32_t)payload);
    for (size_t i = 0; i < payload; i++) {
        f[16 + i] = (uint8_t)(i * 31 + 7);
    }
    return 16 + payload;
}

static void begin(uint8_t kind, const char *id, const uint8_t *data, size_t n, const char *extra)
{
    uint8_t sha[PROTO_SHA256_LEN];
    char sha_hex[65], json[512];
    proto_sha256(data, n, sha);
    hexs(sha, sizeof(sha), sha_hex);
    snprintf(json, sizeof(json), "{\"type\":\"XFER_BEGIN\",\"req_id\":20,\"fields\":{\"kind\":%u,\"id\":\"%s\","
             "\"size\":%zu,\"crc32\":%u,\"sha256\":\"%s\"%s}}", kind, id, n,
             (unsigned)proto_crc32_update(0, data, n), sha_hex, extra);
    xcore_send_json(x, CORE_CHR_TRANSFER, json);
}

static void chunk(uint8_t handle, uint32_t off, const uint8_t *d, size_t n)
{
    static char json[4096], hex[2049];
    hexs(d, n, hex);
    snprintf(json, sizeof(json), "{\"type\":\"XFER_CHUNK\",\"req_id\":0,\"fields\":{\"handle\":%u,\"offset\":%u,"
             "\"data\":\"%s\"}}", handle, (unsigned)off, hex);
    xcore_send_json(x, CORE_CHR_TRANSFER, json);
}

static void test_asset_transfer_with_resume(void)
{
    uint8_t f[300];
    size_t n = theme_asset(f, 184);
    begin(1, "dark", f, n, ",\"asset_type\":3");
    const char *r = msg_at(0, NULL);
    ASSERT_CONTAINS(r, "\"status\":0");
    ASSERT_CONTAINS(r, "\"max_chunk\":493,\"ack_window\":1024");
    uint8_t h = (uint8_t)num_field(r, "handle");
    chunk(h, 0, f, 100);
    TEST_ASSERT_EQUAL_INT(0, x->n_out); /* below the ack window */
    core_disconnect(&x->core);          /* journals offset 100 */
    core_connect(&x->core, 517, true);
    begin(1, "dark", f, n, ",\"asset_type\":3");
    r = msg_at(0, NULL);
    TEST_ASSERT_EQUAL_INT(100, (int)num_field(r, "offset"));
    TEST_ASSERT_EQUAL_UINT32(proto_crc32_update(0, f, 100), (uint32_t)num_field(r, "prefix_crc32"));
    h = (uint8_t)num_field(r, "handle");
    chunk(h, 100, f + 100, n - 100);
    uint8_t chr = 0;
    ASSERT_CONTAINS(msg_at(0, &chr), "\"type\":\"XFER_ACK\",\"req_id\":0,\"fields\":{\"handle\":");
    ASSERT_CONTAINS(msg_at(0, NULL), "\"offset\":200,\"status\":0");
    TEST_ASSERT_EQUAL_UINT8(CORE_CHR_TRANSFER, chr);
    char json[128];
    snprintf(json, sizeof(json), "{\"type\":\"XFER_FINISH\",\"req_id\":21,\"fields\":{\"handle\":%u}}", h);
    xcore_send_json(x, CORE_CHR_TRANSFER, json);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"XFER_FINISH_RSP\",\"req_id\":21,\"fields\":{\"status\":0}}", msg_at(0, NULL));
    xcore_send_json(x, CORE_CHR_TRANSFER, "{\"type\":\"ASSET_LIST\",\"req_id\":22,\"fields\":{}}");
    ASSERT_CONTAINS(msg_at(0, NULL), "\"asset\":[{\"asset_type\":3,\"id\":\"dark\",\"size\":200}]");
}

static void test_ota_through_core(void)
{
    uint8_t img[1000];
    for (size_t i = 0; i < sizeof(img); i++) {
        img[i] = (uint8_t)(i ^ 0x5A);
    }
    x->battery_pct = 10;
    core_poll_battery(&x->core);
    begin(2, "firmware", img, sizeof(img), ",\"version\":\"0.2.0\"");
    ASSERT_CONTAINS(msg_at(0, NULL), "\"status\":22"); /* LOW_BATTERY */
    x->external_power = PROTO_EXTERNAL_POWER_YES;
    begin(2, "firmware", img, sizeof(img), ",\"version\":\"0.1.0\"");
    ASSERT_CONTAINS(msg_at(0, NULL), "\"status\":23"); /* VERSION_REJECTED: same version */
    begin(2, "firmware", img, sizeof(img), ",\"version\":\"0.2.0\"");
    uint8_t h = (uint8_t)num_field(msg_at(0, NULL), "handle");
    chunk(h, 0, img, 493);
    chunk(h, 493, img + 493, 493);
    chunk(h, 986, img + 986, 14);
    TEST_ASSERT_GREATER_THAN(0, x->ota_events[XFER_OTA_EV_PROGRESS]);
    char json[128];
    snprintf(json, sizeof(json), "{\"type\":\"XFER_FINISH\",\"req_id\":23,\"fields\":{\"handle\":%u}}", h);
    xcore_send_json(x, CORE_CHR_TRANSFER, json);
    ASSERT_CONTAINS(msg_at(0, NULL), "\"status\":0");
    TEST_ASSERT_EQUAL_INT(1, x->ota_events[XFER_OTA_EV_REBOOT]);
    TEST_ASSERT_TRUE(x->fw.boot_set);

    /* the new image boots, passes its (empty) self-check and reports updated */
    snprintf(x->version, sizeof(x->version), "0.2.0");
    xcore_reboot(x, true, true);
    TEST_ASSERT_EQUAL_INT(1, x->ota_events[XFER_OTA_EV_MARK_VALID]);
    uint8_t b[64];
    char *s = msg_to_json_str(b, core_read(&x->core, CORE_CHR_STATUS, b, sizeof(b)));
    ASSERT_CONTAINS(s, "\"ota_state\":3");
    free(s);
}

static void test_ota_verify_timeout(void)
{
    x->params.ota_required_checks = 1;
    xcore_reboot(x, false, false);
    x->params.ota_required_checks = 1;
    core_init(&x->core, &x->params);
    core_boot(&x->core, true);
    core_connect(&x->core, 517, true);
    x->now_ms += 60000;
    x->n_out = 0;
    core_tick(&x->core);
    TEST_ASSERT_EQUAL_INT(1, x->ota_events[XFER_OTA_EV_ROLLBACK]);
}

static void test_apps_flushed_periodically(void)
{
    const char *post = "{\"type\":\"NOTIF_POST\",\"req_id\":3,\"fields\":{\"key\":\"k1\",\"app_id\":\"com.a\"}}";
    core_set_utc(&x->core, 1791700000);
    xcore_send_json(x, CORE_CHR_NOTIFICATION, post);
    x->now_ms += 10000;
    xcore_send_json(x, CORE_CHR_NOTIFICATION, post); /* recency updated in RAM only */
    uint32_t before = x->kv.mutations;
    x->now_ms += 30000;
    core_tick(&x->core);
    TEST_ASSERT_EQUAL_UINT32(before, x->kv.mutations);
    x->now_ms += 30000;
    core_tick(&x->core);
    TEST_ASSERT_GREATER_THAN_UINT32(before, x->kv.mutations);
}

/* ---------------------------------------------------------------- clock faces (M8b): option files and the fallback */

static void put_blob(const char *name)
{
    static const uint8_t body[20] = {1};
    TEST_ASSERT_EQUAL_INT(BLOB_OK, x->blob.iface.write(x->blob.iface.ctx, name, 0, body, sizeof(body)));
}

static bool has_blob(const char *name)
{
    uint32_t size;
    return x->blob.iface.stat(x->blob.iface.ctx, name, &size) == BLOB_OK;
}

static size_t face_opts(uint8_t out[32])
{
    const uint8_t *v;
    const size_t n = cfg_get_str(&x->core.cfg, PROTO_CFG_UI_CLOCK_FACE_OPTS, &v);
    memcpy(out, v, n);
    return n;
}

static void test_face_option_values_follow_the_face(void)
{
    uint8_t o[32];
    put_blob("6_me.a");
    put_blob("6_me.b");
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_face_select(&x->core, "me.a"));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_face_option(&x->core, 2, 0));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_face_option(&x->core, 1, 3));
    TEST_ASSERT_EQUAL_size_t(4, face_opts(o));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){2, 0, 1, 3}), o, 4);
    TEST_ASSERT_TRUE(has_blob("opt_me.a"));
    /* another face starts with its defaults, and keeps its own values */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_face_select(&x->core, "me.b"));
    TEST_ASSERT_EQUAL_size_t(0, face_opts(o));
    TEST_ASSERT_FALSE(has_blob("opt_me.b"));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_face_option(&x->core, 2, 1));
    TEST_ASSERT_TRUE(has_blob("opt_me.b"));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_face_select(&x->core, "me.a"));
    TEST_ASSERT_EQUAL_size_t(4, face_opts(o));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){2, 0, 1, 3}), o, 4);
    /* a change of an option replaces its pair; Classic has no values and no file */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_face_option(&x->core, 2, 1));
    TEST_ASSERT_EQUAL_size_t(4, face_opts(o));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){1, 3, 2, 1}), o, 4);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_face_select(&x->core, ""));
    TEST_ASSERT_EQUAL_size_t(0, face_opts(o));
    /* a write of both keys in one CONFIG_SET: the values given win over the new face's stored ones */
    xcore_send_json(x, CORE_CHR_CONFIG, "{\"type\":\"CONFIG_SET\",\"req_id\":1,\"fields\":{\"entry\":["
                    "{\"key\":67,\"value\":\"6d652e62\"},{\"key\":68,\"value\":\"0900\"}]}}");
    TEST_ASSERT_EQUAL_size_t(2, face_opts(o));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){9, 0}), o, 2);
    TEST_ASSERT_TRUE(has_blob("opt_me.b"));
}

static void test_deleting_a_face_takes_its_options_and_the_selection(void)
{
    const uint8_t *v;
    uint8_t o[32];
    put_blob("6_me.a");
    put_blob("6_me.b");
    (void)core_face_select(&x->core, "me.b");
    (void)core_face_option(&x->core, 2, 1);
    (void)core_face_select(&x->core, "me.a");
    (void)core_face_option(&x->core, 2, 0);
    TEST_ASSERT_TRUE(has_blob("opt_me.a") && has_blob("opt_me.b"));
    /* a face that is not selected goes with its file; the selection stays */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_face_delete(&x->core, "me.b"));
    TEST_ASSERT_FALSE(has_blob("6_me.b") || has_blob("opt_me.b"));
    TEST_ASSERT_EQUAL_size_t(4, cfg_get_str(&x->core.cfg, PROTO_CFG_UI_CLOCK_FACE_ID, &v)); /* "me.a" */
    TEST_ASSERT_TRUE(has_blob("opt_me.a"));
    /* the selected face: Classic is selected again, with no values */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, core_face_delete(&x->core, "me.a"));
    TEST_ASSERT_FALSE(has_blob("6_me.a") || has_blob("opt_me.a"));
    TEST_ASSERT_EQUAL_size_t(0, cfg_get_str(&x->core.cfg, PROTO_CFG_UI_CLOCK_FACE_ID, &v));
    TEST_ASSERT_EQUAL_size_t(0, face_opts(o));
    TEST_ASSERT_EQUAL_INT(PROTO_NOT_FOUND, core_face_delete(&x->core, "me.a"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_capabilities_read);
    RUN_TEST(test_status_read);
    RUN_TEST(test_battery_unknown);
    RUN_TEST(test_device_status_on_change);
    RUN_TEST(test_wifi_and_weather_status);
    RUN_TEST(test_wifi_set_platform_error_and_events);
    RUN_TEST(test_on_setting_only_for_the_phone);
    RUN_TEST(test_no_device_status_for_notif_count);
    RUN_TEST(test_disconnected_sends_nothing);
    RUN_TEST(test_config_changed_after_response);
    RUN_TEST(test_app_seen_after_response);
    RUN_TEST(test_ancs_app_seen_outside_request);
    RUN_TEST(test_dismiss_event);
    RUN_TEST(test_dismiss_reports_the_ancs_uid);
    RUN_TEST(test_config_set_on_the_board);
    RUN_TEST(test_config_set_any_type_on_the_board);
    RUN_TEST(test_time_sync_age);
    RUN_TEST(test_time_sync_sets_clock);
    RUN_TEST(test_clock_across_ms_wrap);
    RUN_TEST(test_rtc_sets_clock_at_boot_and_time_sync_writes_it);
    RUN_TEST(test_no_rtc_clock_lost_at_reboot);
    RUN_TEST(test_read_only_and_unknown_characteristics);
    RUN_TEST(test_mtu_and_max_chunk);
    RUN_TEST(test_asset_transfer_with_resume);
    RUN_TEST(test_face_option_values_follow_the_face);
    RUN_TEST(test_deleting_a_face_takes_its_options_and_the_selection);
    RUN_TEST(test_ota_through_core);
    RUN_TEST(test_ota_verify_timeout);
    RUN_TEST(test_apps_flushed_periodically);
    return UNITY_END();
}
