/* config_store and kv_mem: protocol.md 5 semantics, persistence, migration, storage failures. */
#include <stdlib.h>
#include <string.h>

#include "epager/cfg_store.h"
#include "test_support.h"
#include "unity.h"

static kv_mem_t kv;
static cfg_store_t cfg;
static int32_t power;

/* Change-event recorder */
static int n_events;
static uint32_t ev_rev;
static uint16_t ev_keys[PROTO_CFG_KEY_COUNT];
static size_t ev_n;
static uint32_t synced_utc;

static void on_change(void *ctx, uint32_t rev, const uint16_t *keys, size_t n)
{
    (void)ctx;
    n_events++;
    ev_rev = rev;
    ev_n = n;
    if (n) {
        memcpy(ev_keys, keys, n * sizeof(keys[0]));
    }
}

static void set_time(void *ctx, uint32_t utc)
{
    (void)ctx;
    synced_utc = utc;
}

static void boot(uint8_t variant)
{
    cfg_params_t p = {.kv = &kv.iface, .variant = variant, .on_change = on_change, .set_time = set_time};
    cfg_init(&cfg, &p);
}

void setUp(void)
{
    kv_mem_init(&kv);
    n_events = 0;
    ev_n = 0;
    synced_utc = 0;
    boot(PROTO_VARIANT_PAPER);
}
void tearDown(void) {}

static proto_status_t set1(uint16_t key, const void *val, size_t len)
{
    cfg_entry_t e = {key, val, len};
    proto_status_t ks;
    return cfg_set(&cfg, &e, 1, NULL, &ks);
}

static proto_status_t set_u8(uint16_t key, uint8_t v) { return set1(key, &v, 1); }
static proto_status_t set_u16(uint16_t key, uint16_t v)
{
    uint8_t b[2];
    proto_wr16(b, v);
    return set1(key, b, 2);
}

/* Run a request through cfg_handle; returns the response as compact JSON (static buffer). */
static const char *handle(const char *req_json)
{
    static char out[2048];
    uint8_t buf[1024], rsp[PROTO_LIMIT_MAX_MESSAGE_CONFIG];
    proto_msg_t m = parse_json_msg(req_json, buf, sizeof(buf));
    proto_writer_t w;
    proto_writer_init(&w, rsp, sizeof(rsp));
    TEST_ASSERT_TRUE(cfg_handle(&cfg, &m, &w));
    TEST_ASSERT_TRUE(proto_writer_ok(&w));
    char *s = msg_to_json_str(rsp, w.len);
    snprintf(out, sizeof(out), "%s", s);
    free(s);
    return out;
}

/* ---------------------------------------------------------------- defaults and variants */

static void test_defaults_on_empty_storage(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, cfg_revision(&cfg));
    for (size_t i = 0; i < proto_cfg_keys_count; i++) {
        const proto_cfg_key_desc_t *k = &proto_cfg_keys[i];
        if (k->type == PROTO_FT_BYTES) {
            const uint8_t *s;
            TEST_ASSERT_EQUAL_UINT(0, cfg_get_str(&cfg, k->id, &s));
        } else if (k->type == PROTO_FT_STR) {
            const uint8_t *s;
            TEST_ASSERT_EQUAL_UINT(strlen(k->default_str), cfg_get_str(&cfg, k->id, &s));
        } else {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(k->default_int, cfg_get_int(&cfg, k->id), k->name);
        }
    }
    TEST_ASSERT_EQUAL_INT32(0, cfg_get_int(&cfg, 999));
    const uint8_t *s;
    TEST_ASSERT_EQUAL_UINT(0, cfg_get_str(&cfg, 999, &s));
    TEST_ASSERT_EQUAL_UINT(0, kv_mem_count(&kv)); /* nothing written until a change */
}

static void test_variant_specific_keys(void)
{
    uint8_t v[64];
    size_t n;
    TEST_ASSERT_TRUE(cfg_encode_value(&cfg, PROTO_CFG_DISPLAY_FULL_REFRESH_EVERY, v, &n));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT8(30, v[0]);
    boot(PROTO_VARIANT_LCD);
    TEST_ASSERT_FALSE(cfg_encode_value(&cfg, PROTO_CFG_DISPLAY_FULL_REFRESH_EVERY, v, &n));
    uint8_t five = 5;
    cfg_entry_t e = {PROTO_CFG_DISPLAY_FULL_REFRESH_EVERY, &five, 1};
    proto_status_t ks;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, cfg_set(&cfg, &e, 1, NULL, &ks)); /* skipped, not an error */
    TEST_ASSERT_EQUAL_INT(PROTO_UNKNOWN_KEY, ks);
    TEST_ASSERT_EQUAL_INT32(30, cfg_get_int(&cfg, PROTO_CFG_DISPLAY_FULL_REFRESH_EVERY)); /* default */
    TEST_ASSERT_FALSE(cfg_encode_value(&cfg, 999, v, &n));
}

/* ---------------------------------------------------------------- CONFIG_SET semantics */

static void test_batch_commit_persists_and_notifies(void)
{
    uint8_t tz[2], lang = PROTO_LANGUAGE_VI, start[2];
    proto_wr16(tz, (uint16_t)-300);
    proto_wr16(start, 1380);
    cfg_entry_t e[] = {{PROTO_CFG_TIME_TZ_OFFSET_MIN, tz, 2}, {PROTO_CFG_UI_LANGUAGE, &lang, 1},
                       {PROTO_CFG_QUIET_START_MIN, start, 2}};
    proto_status_t ks[3];
    TEST_ASSERT_EQUAL_INT(PROTO_OK, cfg_set(&cfg, e, 3, NULL, ks));
    TEST_ASSERT_EQUAL_UINT32(1, cfg_revision(&cfg));
    TEST_ASSERT_EQUAL_INT(1, n_events);
    TEST_ASSERT_EQUAL_UINT32(1, ev_rev);
    const uint16_t want[] = {PROTO_CFG_UI_LANGUAGE, PROTO_CFG_QUIET_START_MIN, PROTO_CFG_TIME_TZ_OFFSET_MIN};
    TEST_ASSERT_EQUAL_UINT(3, ev_n);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want, ev_keys, 3); /* ascending id, not wire order */
    TEST_ASSERT_EQUAL_INT32(-300, cfg_get_int(&cfg, PROTO_CFG_TIME_TZ_OFFSET_MIN));

    boot(PROTO_VARIANT_PAPER); /* simulated reboot */
    TEST_ASSERT_EQUAL_UINT32(1, cfg_revision(&cfg));
    TEST_ASSERT_EQUAL_INT32(-300, cfg_get_int(&cfg, PROTO_CFG_TIME_TZ_OFFSET_MIN));
    TEST_ASSERT_EQUAL_INT32(PROTO_LANGUAGE_VI, cfg_get_int(&cfg, PROTO_CFG_UI_LANGUAGE));
    TEST_ASSERT_EQUAL_INT32(1380, cfg_get_int(&cfg, PROTO_CFG_QUIET_START_MIN));
}

static void test_no_change_keeps_revision(void)
{
    TEST_ASSERT_EQUAL_INT(PROTO_OK, set_u8(PROTO_CFG_UI_LANGUAGE, PROTO_LANGUAGE_EN)); /* = default */
    TEST_ASSERT_EQUAL_UINT32(0, cfg_revision(&cfg));
    TEST_ASSERT_EQUAL_INT(0, n_events);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, set_u8(PROTO_CFG_UI_LANGUAGE, PROTO_LANGUAGE_VI));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, set_u8(PROTO_CFG_UI_LANGUAGE, PROTO_LANGUAGE_VI));
    TEST_ASSERT_EQUAL_UINT32(1, cfg_revision(&cfg));
    TEST_ASSERT_EQUAL_INT(1, n_events);
}

static void test_unknown_keys_skipped_and_reported(void)
{
    uint8_t one = 1;
    cfg_entry_t e[] = {{999, &one, 1}, {PROTO_CFG_QUIET_ENABLED, &one, 1}, {0x7FFF, NULL, 0}};
    proto_status_t ks[3];
    TEST_ASSERT_EQUAL_INT(PROTO_OK, cfg_set(&cfg, e, 3, NULL, ks));
    TEST_ASSERT_EQUAL_INT(PROTO_UNKNOWN_KEY, ks[0]);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, ks[1]);
    TEST_ASSERT_EQUAL_INT(PROTO_UNKNOWN_KEY, ks[2]);
    TEST_ASSERT_EQUAL_INT32(1, cfg_get_int(&cfg, PROTO_CFG_QUIET_ENABLED));
    /* only unknown keys: OK, nothing changes */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, cfg_set(&cfg, e, 1, NULL, ks));
    TEST_ASSERT_EQUAL_UINT32(1, cfg_revision(&cfg));
}

static void test_partial_batch_rejected(void)
{
    uint8_t vi = 1, bad_orient = 4, mask[2], big[33], bad_utf8[] = {0xC3, 0x28}, wide[2] = {1, 0};
    proto_wr16(mask, 0x0F0F);
    memset(big, 'a', sizeof(big));
    struct {
        cfg_entry_t bad;
        proto_status_t want;
    } cases[] = {
        {{PROTO_CFG_DISPLAY_ORIENTATION, &bad_orient, 1}, PROTO_OUT_OF_RANGE},
        {{PROTO_CFG_UI_LANGUAGE, wide, 2}, PROTO_TYPE_MISMATCH},
        {{PROTO_CFG_UI_THEME_ID, big, sizeof(big)}, PROTO_TOO_LARGE},
        {{PROTO_CFG_UI_FONT_ID, bad_utf8, sizeof(bad_utf8)}, PROTO_INVALID_UTF8},
        {{PROTO_CFG_NOTIF_MIN_PRIORITY, (const uint8_t *)"\x04", 1}, PROTO_OUT_OF_RANGE}, /* enum */
        {{PROTO_CFG_DISPLAY_SHOW_BODY, (const uint8_t *)"\x02", 1}, PROTO_OUT_OF_RANGE},  /* bool */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cfg_entry_t e[] = {{PROTO_CFG_UI_LANGUAGE, &vi, 1}, {999, &vi, 1}, cases[i].bad,
                           {PROTO_CFG_NOTIF_CATEGORY_MASK, mask, 2}};
        TEST_ASSERT_EQUAL_INT(cases[i].want, cfg_set(&cfg, e, 4, NULL, NULL));
    }
    TEST_ASSERT_EQUAL_UINT32(0, cfg_revision(&cfg));
    TEST_ASSERT_EQUAL_INT(0, n_events);
    TEST_ASSERT_EQUAL_INT32(PROTO_LANGUAGE_EN, cfg_get_int(&cfg, PROTO_CFG_UI_LANGUAGE));
    TEST_ASSERT_EQUAL_INT32(4095, cfg_get_int(&cfg, PROTO_CFG_NOTIF_CATEGORY_MASK));

    /* the first failing key in wire order decides the status */
    cfg_entry_t two[] = {{PROTO_CFG_UI_LANGUAGE, wide, 2}, {PROTO_CFG_DISPLAY_ORIENTATION, &bad_orient, 1}};
    TEST_ASSERT_EQUAL_INT(PROTO_TYPE_MISMATCH, cfg_set(&cfg, two, 2, NULL, NULL));
}

static void test_expected_revision(void)
{
    uint8_t one = 1;
    cfg_entry_t e = {PROTO_CFG_QUIET_ENABLED, &one, 1};
    uint32_t rev = 5;
    TEST_ASSERT_EQUAL_INT(PROTO_REVISION_CONFLICT, cfg_set(&cfg, &e, 1, &rev, NULL));
    /* key errors are reported before the revision check */
    uint8_t bad = 9;
    cfg_entry_t b = {PROTO_CFG_DISPLAY_ORIENTATION, &bad, 1};
    TEST_ASSERT_EQUAL_INT(PROTO_OUT_OF_RANGE, cfg_set(&cfg, &b, 1, &rev, NULL));
    rev = 0;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, cfg_set(&cfg, &e, 1, &rev, NULL));
    TEST_ASSERT_EQUAL_INT(PROTO_REVISION_CONFLICT, cfg_set(&cfg, &e, 1, &rev, NULL)); /* now 1 */
}

static void test_duplicate_key_last_wins(void)
{
    uint8_t a = 1, b = 3;
    cfg_entry_t e[] = {{PROTO_CFG_DISPLAY_ORIENTATION, &a, 1}, {PROTO_CFG_DISPLAY_ORIENTATION, &b, 1}};
    TEST_ASSERT_EQUAL_INT(PROTO_OK, cfg_set(&cfg, e, 2, NULL, NULL));
    TEST_ASSERT_EQUAL_INT32(3, cfg_get_int(&cfg, PROTO_CFG_DISPLAY_ORIENTATION));
    TEST_ASSERT_EQUAL_UINT(1, ev_n);
}

static void test_batch_limit(void)
{
    static cfg_entry_t e[CFG_MAX_BATCH + 1];
    uint8_t one = 1;
    for (size_t i = 0; i <= CFG_MAX_BATCH; i++) {
        e[i] = (cfg_entry_t){PROTO_CFG_QUIET_ENABLED, &one, 1};
    }
    TEST_ASSERT_EQUAL_INT(PROTO_TOO_LARGE, cfg_set(&cfg, e, CFG_MAX_BATCH + 1, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, cfg_set(&cfg, e, CFG_MAX_BATCH, NULL, NULL));
}

static void test_storage_failure_changes_nothing(void)
{
    power = 0;
    kv.power = &power;
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, set_u8(PROTO_CFG_UI_LANGUAGE, PROTO_LANGUAGE_VI));
    TEST_ASSERT_EQUAL_INT32(PROTO_LANGUAGE_EN, cfg_get_int(&cfg, PROTO_CFG_UI_LANGUAGE));
    TEST_ASSERT_EQUAL_UINT32(0, cfg_revision(&cfg));
    TEST_ASSERT_EQUAL_INT(0, n_events);
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, cfg_bump_revision(&cfg));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, cfg_reset(&cfg)); /* nothing to reset: no write */
    power = 1;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, set_u8(PROTO_CFG_UI_LANGUAGE, PROTO_LANGUAGE_VI));
    kv.power = NULL;
}

static void test_reset_and_bump(void)
{
    TEST_ASSERT_EQUAL_INT(PROTO_OK, cfg_reset(&cfg)); /* nothing to reset */
    TEST_ASSERT_EQUAL_UINT32(0, cfg_revision(&cfg));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, set_u16(PROTO_CFG_QUIET_END_MIN, 0));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, set_u8(PROTO_CFG_NOTIF_SHOW_SILENT, 0));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, cfg_reset(&cfg));
    TEST_ASSERT_EQUAL_UINT32(3, cfg_revision(&cfg));
    const uint16_t want[] = {PROTO_CFG_NOTIF_SHOW_SILENT, PROTO_CFG_QUIET_END_MIN};
    TEST_ASSERT_EQUAL_UINT(2, ev_n);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want, ev_keys, 2);
    TEST_ASSERT_EQUAL_INT32(420, cfg_get_int(&cfg, PROTO_CFG_QUIET_END_MIN));

    TEST_ASSERT_EQUAL_INT(PROTO_OK, cfg_bump_revision(&cfg));
    TEST_ASSERT_EQUAL_UINT32(4, ev_rev);
    TEST_ASSERT_EQUAL_UINT(0, ev_n);
    boot(PROTO_VARIANT_PAPER);
    TEST_ASSERT_EQUAL_UINT32(4, cfg_revision(&cfg));
}

static void test_string_keys(void)
{
    TEST_ASSERT_EQUAL_INT(PROTO_OK, set1(PROTO_CFG_UI_THEME_ID, "night", 5));
    const uint8_t *s;
    TEST_ASSERT_EQUAL_UINT(5, cfg_get_str(&cfg, PROTO_CFG_UI_THEME_ID, &s));
    TEST_ASSERT_EQUAL_MEMORY("night", s, 5);
    char full[32 + 1]; /* ui.font_id max_len */
    memset(full, 'z', sizeof(full));
    TEST_ASSERT_EQUAL_INT(PROTO_TOO_LARGE, set1(PROTO_CFG_UI_FONT_ID, full, sizeof(full)));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, set1(PROTO_CFG_UI_FONT_ID, full, 32));
    boot(PROTO_VARIANT_PAPER);
    TEST_ASSERT_EQUAL_UINT(32, cfg_get_str(&cfg, PROTO_CFG_UI_FONT_ID, &s));

    /* the longest string key: weather.city on LCD, kept across a reboot */
    boot(PROTO_VARIANT_LCD);
    char city[PROTO_CFG_STR_MAX_LEN];
    memset(city, 'c', sizeof(city));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, set1(PROTO_CFG_WEATHER_CITY, city, sizeof(city)));
    boot(PROTO_VARIANT_LCD);
    TEST_ASSERT_EQUAL_UINT(PROTO_CFG_STR_MAX_LEN, cfg_get_str(&cfg, PROTO_CFG_WEATHER_CITY, &s));
    TEST_ASSERT_EQUAL_MEMORY(city, s, sizeof(city));
    boot(PROTO_VARIANT_PAPER);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, set1(PROTO_CFG_UI_THEME_ID, "", 0)); /* back to the default */
    TEST_ASSERT_EQUAL_UINT(0, cfg_get_str(&cfg, PROTO_CFG_UI_THEME_ID, &s));
}

/* ---------------------------------------------------------------- stored record */

static size_t stored(uint8_t *b, size_t cap)
{
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(KV_OK, kv.iface.get(kv.iface.ctx, CFG_KV_KEY, b, cap, &n));
    return n;
}

static void put_record(uint32_t version, uint32_t revision, const uint8_t *entries, size_t n, bool good_crc)
{
    uint8_t b[256] = {'E', 'C', 1, 0};
    proto_wr32(b + 4, version);
    proto_wr32(b + 8, revision);
    memcpy(b + 12, entries, n);
    proto_wr32(b + 12 + n, proto_crc32_update(0, b, 12 + n) ^ (good_crc ? 0 : 1));
    TEST_ASSERT_EQUAL_INT(KV_OK, kv.iface.set(kv.iface.ctx, CFG_KV_KEY, b, 16 + n));
}

static void test_only_non_default_values_stored(void)
{
    uint8_t b[512];
    TEST_ASSERT_EQUAL_INT(PROTO_OK, set_u8(PROTO_CFG_UI_LANGUAGE, PROTO_LANGUAGE_VI));
    TEST_ASSERT_EQUAL_UINT(16 + 4, stored(b, sizeof(b)));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, set_u8(PROTO_CFG_UI_LANGUAGE, PROTO_LANGUAGE_EN));
    TEST_ASSERT_EQUAL_UINT(16, stored(b, sizeof(b))); /* header + crc only */
    TEST_ASSERT_EQUAL_UINT32(2, cfg_revision(&cfg));
}

static void test_corrupt_record_gives_defaults(void)
{
    const uint8_t e[] = {1, 0, 1, 1}; /* ui.language = vi */
    put_record(PROTO_CONTRACT_VERSION_PACKED, 7, e, sizeof(e), false);
    boot(PROTO_VARIANT_PAPER);
    TEST_ASSERT_EQUAL_UINT32(0, cfg_revision(&cfg));
    TEST_ASSERT_EQUAL_INT32(PROTO_LANGUAGE_EN, cfg_get_int(&cfg, PROTO_CFG_UI_LANGUAGE));

    put_record(PROTO_CONTRACT_VERSION_PACKED, 7, e, sizeof(e), true);
    boot(PROTO_VARIANT_PAPER);
    TEST_ASSERT_EQUAL_UINT32(7, cfg_revision(&cfg));
    TEST_ASSERT_EQUAL_INT32(PROTO_LANGUAGE_VI, cfg_get_int(&cfg, PROTO_CFG_UI_LANGUAGE));

    const uint8_t short_rec[] = {'E', 'C', 1};
    kv.iface.set(kv.iface.ctx, CFG_KV_KEY, short_rec, sizeof(short_rec));
    boot(PROTO_VARIANT_PAPER);
    TEST_ASSERT_EQUAL_UINT32(0, cfg_revision(&cfg));

    uint8_t huge[KV_MEM_MAX_VALUE] = {'E', 'C', 1};
    kv.iface.set(kv.iface.ctx, CFG_KV_KEY, huge, sizeof(huge)); /* bad crc */
    boot(PROTO_VARIANT_PAPER);
    TEST_ASSERT_EQUAL_UINT32(0, cfg_revision(&cfg));
}

static void test_migration_drops_stale_values(void)
{
    const uint8_t e[] = {
        0xE7, 0x03, 1, 5,       /* unknown key 999: dropped */
        2, 0, 1, 9,             /* display.orientation = 9: no longer valid, default */
        3, 0, 1, 20,            /* full_refresh_every = 20 */
        17, 0, 2, 0x0F, 0x00,   /* category mask = 0x000F */
        33, 0, 9, 1,            /* length runs past the end: parsing stops */
    };
    put_record(PROTO_CONTRACT_VERSION_PACKED, 3, e, sizeof(e), true);
    boot(PROTO_VARIANT_PAPER);
    TEST_ASSERT_EQUAL_UINT32(3, cfg_revision(&cfg));
    TEST_ASSERT_EQUAL_INT32(0, cfg_get_int(&cfg, PROTO_CFG_DISPLAY_ORIENTATION));
    TEST_ASSERT_EQUAL_INT32(20, cfg_get_int(&cfg, PROTO_CFG_DISPLAY_FULL_REFRESH_EVERY));
    TEST_ASSERT_EQUAL_INT32(0x000F, cfg_get_int(&cfg, PROTO_CFG_NOTIF_CATEGORY_MASK));
    TEST_ASSERT_EQUAL_INT32(1320, cfg_get_int(&cfg, PROTO_CFG_QUIET_START_MIN));

    /* the same record on a variant without the key */
    boot(PROTO_VARIANT_LCD);
    TEST_ASSERT_EQUAL_INT32(30, cfg_get_int(&cfg, PROTO_CFG_DISPLAY_FULL_REFRESH_EVERY));

    /* written by firmware older than the keys' `since`: those values are not trusted */
    put_record(0x000001, 3, e, sizeof(e), true);
    boot(PROTO_VARIANT_PAPER);
    TEST_ASSERT_EQUAL_INT32(30, cfg_get_int(&cfg, PROTO_CFG_DISPLAY_FULL_REFRESH_EVERY));
    TEST_ASSERT_EQUAL_INT32(4095, cfg_get_int(&cfg, PROTO_CFG_NOTIF_CATEGORY_MASK));
    TEST_ASSERT_EQUAL_UINT32(3, cfg_revision(&cfg));
}

/* ---------------------------------------------------------------- message adapter */

static void test_msg_config_get(void)
{
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"CONFIG_GET_RSP\",\"req_id\":3,\"fields\":{\"status\":0,\"revision\":0,\"entry\":["
        "{\"key\":17,\"value\":\"ff0f\"},{\"key\":2,\"value\":\"00\"},{\"key\":64,\"value\":\"\"}]}}",
        handle("{\"type\":\"CONFIG_GET\",\"req_id\":3,\"fields\":{\"key\":[17,999,2,17,64]}}"));
    const char *all = handle("{\"type\":\"CONFIG_GET\",\"req_id\":4}");
    TEST_ASSERT_NOT_NULL(strstr(all, "{\"key\":3,\"value\":\"1e\"}"));
    TEST_ASSERT_NOT_NULL(strstr(all, "{\"key\":65,\"value\":\"\"},{\"key\":66,\"value\":\"\"},{\"key\":67,\"value\":\"\"},"
                                     "{\"key\":68,\"value\":\"\"},{\"key\":80,\"value\":\"01\"},"
                                     "{\"key\":81,\"value\":\"\"},{\"key\":82,\"value\":\"3c00\"},{\"key\":83,\"value\":\"01\"}]"));
    boot(PROTO_VARIANT_LCD);
    TEST_ASSERT_NULL(strstr(handle("{\"type\":\"CONFIG_GET\",\"req_id\":4}"), "{\"key\":3,"));
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"CONFIG_GET_RSP\",\"req_id\":5,\"fields\":{\"status\":0,\"revision\":0}}",
                             handle("{\"type\":\"CONFIG_GET\",\"req_id\":5,\"fields\":{\"key\":[3]}}"));
}

static void test_msg_config_set(void)
{
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"CONFIG_SET_RSP\",\"req_id\":7,\"fields\":{\"status\":0,\"revision\":1,\"key_status\":["
        "{\"key\":1,\"status\":0},{\"key\":999,\"status\":9}]}}",
        handle("{\"type\":\"CONFIG_SET\",\"req_id\":7,\"fields\":{\"entry\":["
               "{\"key\":1,\"value\":\"01\"},{\"key\":999,\"value\":\"01\"}]}}"));
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"CONFIG_SET_RSP\",\"req_id\":8,\"fields\":{\"status\":12}}",
                             handle("{\"type\":\"CONFIG_SET\",\"req_id\":8,\"fields\":{\"expected_revision\":0,"
                                    "\"entry\":[{\"key\":1,\"value\":\"00\"}]}}"));
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"CONFIG_SET_RSP\",\"req_id\":9,\"fields\":{\"status\":8}}",
                             handle("{\"type\":\"CONFIG_SET\",\"req_id\":9,\"fields\":{\"entry\":["
                                    "{\"key\":1,\"value\":\"00\"},{\"key\":33,\"value\":\"a005\"}]}}"));
    TEST_ASSERT_EQUAL_INT32(PROTO_LANGUAGE_VI, cfg_get_int(&cfg, PROTO_CFG_UI_LANGUAGE));

    char big[4096] = "{\"type\":\"CONFIG_SET\",\"req_id\":10,\"fields\":{\"entry\":[";
    for (unsigned i = 0; i <= CFG_MAX_BATCH; i++) {
        strcat(big, i ? ",{\"key\":32,\"value\":\"00\"}" : "{\"key\":32,\"value\":\"00\"}");
    }
    strcat(big, "]}}");
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"CONFIG_SET_RSP\",\"req_id\":10,\"fields\":{\"status\":5}}", handle(big));
}

static void test_msg_reset_and_time_sync(void)
{
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"TIME_SYNC_RSP\",\"req_id\":1,\"fields\":{\"status\":0}}",
                             handle("{\"type\":\"TIME_SYNC\",\"req_id\":1,\"fields\":{\"utc_s\":1791700215,"
                                    "\"tz_offset_min\":420}}"));
    TEST_ASSERT_EQUAL_UINT32(1791700215u, synced_utc);
    TEST_ASSERT_EQUAL_INT32(420, cfg_get_int(&cfg, PROTO_CFG_TIME_TZ_OFFSET_MIN));
    TEST_ASSERT_EQUAL_UINT(1, ev_n);
    TEST_ASSERT_EQUAL_UINT16(PROTO_CFG_TIME_TZ_OFFSET_MIN, ev_keys[0]);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"CONFIG_RESET_RSP\",\"req_id\":2,\"fields\":{\"status\":0,\"revision\":2}}",
                             handle("{\"type\":\"CONFIG_RESET\",\"req_id\":2}"));
    power = 0;
    kv.power = &power;
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"TIME_SYNC_RSP\",\"req_id\":3,\"fields\":{\"status\":27}}",
                             handle("{\"type\":\"TIME_SYNC\",\"req_id\":3,\"fields\":{\"utc_s\":5,"
                                    "\"tz_offset_min\":-60}}"));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, set_u16(PROTO_CFG_QUIET_END_MIN, 420)); /* no change: no write needed */
    kv.power = NULL;
    set_u16(PROTO_CFG_QUIET_END_MIN, 0);
    power = 0;
    kv.power = &power;
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"CONFIG_RESET_RSP\",\"req_id\":4,\"fields\":{\"status\":27}}",
                             handle("{\"type\":\"CONFIG_RESET\",\"req_id\":4}"));
    kv.power = NULL;

    uint8_t buf[16], rsp[16];
    proto_msg_t m = parse_json_msg("{\"type\":\"PING\"}", buf, sizeof(buf));
    proto_writer_t w;
    proto_writer_init(&w, rsp, sizeof(rsp));
    TEST_ASSERT_FALSE(cfg_handle(&cfg, &m, &w));
}

static void test_changed_event_matches_vector(void)
{
    cJSON *doc = load_vectors("messages.json");
    const cJSON *c;
    int found = 0;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(doc, "cases")) {
        const char *name = jstr(c, "name");
        uint8_t out[64];
        proto_writer_t w;
        proto_writer_init(&w, out, sizeof(out));
        char hex[129];
        if (strcmp(name, "event_config_changed") == 0) {
            const uint16_t keys[] = {1, 33};
            cfg_write_changed_event(&w, 8, keys, 2);
        } else if (strcmp(name, "event_config_changed_policy_only") == 0) {
            cfg_write_changed_event(&w, 9, NULL, 0);
        } else {
            continue;
        }
        found++;
        hex_encode(out, w.len, hex);
        TEST_ASSERT_EQUAL_STRING(jstr(c, "hex"), hex);
    }
    cJSON_Delete(doc);
    TEST_ASSERT_EQUAL_INT(2, found);
}

/* ---------------------------------------------------------------- kv_mem */

static void test_kv_mem(void)
{
    kv_storage_t *s = &kv.iface;
    uint8_t buf[4];
    size_t n;
    TEST_ASSERT_EQUAL_INT(KV_NOT_FOUND, s->get(s->ctx, "a", buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_INT(KV_OK, s->set(s->ctx, "a", "hello", 5));
    TEST_ASSERT_EQUAL_INT(KV_TOO_SMALL, s->get(s->ctx, "a", buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_INT(KV_ERR, s->set(s->ctx, "0123456789abcdef", "x", 1)); /* key > 15 */
    static uint8_t big[KV_MEM_MAX_VALUE + 1];
    TEST_ASSERT_EQUAL_INT(KV_ERR, s->set(s->ctx, "b", big, sizeof(big)));
    TEST_ASSERT_EQUAL_INT(KV_OK, s->erase(s->ctx, "nope"));
    TEST_ASSERT_EQUAL_INT(KV_OK, s->erase(s->ctx, "a"));
    TEST_ASSERT_EQUAL_UINT(0, kv_mem_count(&kv));
    char key[16];
    for (unsigned i = 0; i < KV_MEM_MAX_ENTRIES; i++) {
        snprintf(key, sizeof(key), "k%u", i);
        TEST_ASSERT_EQUAL_INT(KV_OK, s->set(s->ctx, key, &i, sizeof(i)));
    }
    TEST_ASSERT_EQUAL_INT(KV_ERR, s->set(s->ctx, "full", "x", 1));
    TEST_ASSERT_EQUAL_INT(KV_OK, s->set(s->ctx, "k3", "y", 1)); /* overwrite works when full */
    power = 1;
    kv.power = &power;
    TEST_ASSERT_EQUAL_INT(KV_OK, s->erase(s->ctx, "k1"));
    TEST_ASSERT_EQUAL_INT(KV_ERR, s->erase(s->ctx, "k2"));
    TEST_ASSERT_EQUAL_INT(KV_ERR, s->set(s->ctx, "k2", "z", 1));
    TEST_ASSERT_EQUAL_INT(KV_OK, s->get(s->ctx, "k2", buf, sizeof(buf), &n)); /* reads still work */
    kv.power = NULL;
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_on_empty_storage);
    RUN_TEST(test_variant_specific_keys);
    RUN_TEST(test_batch_commit_persists_and_notifies);
    RUN_TEST(test_no_change_keeps_revision);
    RUN_TEST(test_unknown_keys_skipped_and_reported);
    RUN_TEST(test_partial_batch_rejected);
    RUN_TEST(test_expected_revision);
    RUN_TEST(test_duplicate_key_last_wins);
    RUN_TEST(test_batch_limit);
    RUN_TEST(test_storage_failure_changes_nothing);
    RUN_TEST(test_reset_and_bump);
    RUN_TEST(test_string_keys);
    RUN_TEST(test_only_non_default_values_stored);
    RUN_TEST(test_corrupt_record_gives_defaults);
    RUN_TEST(test_migration_drops_stale_values);
    RUN_TEST(test_msg_config_get);
    RUN_TEST(test_msg_config_set);
    RUN_TEST(test_msg_reset_and_time_sync);
    RUN_TEST(test_changed_event_matches_vector);
    RUN_TEST(test_kv_mem);
    return UNITY_END();
}
