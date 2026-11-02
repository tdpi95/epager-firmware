/* Portable helpers of the BLE glue (components/ble_core/src/ble_util.c): advertising bytes against gatt.json,
 * the device name, the passkey, pairing policy, Apple's connection and advertising rules, the connection
 * policy over time and the record FIFO. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "epager/ble_util.h"
#include "epager/proto_gen.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* "a08a0001-0c0a-4214-8b47-6c1925da5fca" -> 16 bytes, least significant first (as on the air) */
static void uuid_le(const char *s, uint8_t out[16])
{
    uint8_t be[16];
    size_t n = 0;
    for (const char *p = s; *p && n < 16; p++) {
        if (*p == '-') {
            continue;
        }
        unsigned v;
        char h[3] = {p[0], p[1], 0};
        TEST_ASSERT_EQUAL_INT(1, sscanf(h, "%2x", &v));
        be[n++] = (uint8_t)v;
        p++;
    }
    TEST_ASSERT_EQUAL_UINT(16, n);
    for (size_t i = 0; i < 16; i++) {
        out[i] = be[15 - i];
    }
}

static cJSON *gatt(void)
{
    FILE *f = fopen(GATT_JSON, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "contracts/gatt.json");
    static char buf[16384];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *j = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(j);
    return j;
}

static void test_name(void)
{
    char name[BLEU_NAME_MAX];
    const uint8_t addr[6] = {0xEF, 0xCD, 0xAB, 0x90, 0x78, 0x56}; /* MAC 56:78:90:AB:CD:EF */
    bleu_device_name(name, addr);
    TEST_ASSERT_EQUAL_STRING("ePager-CDEF", name);
    const uint8_t zero[6] = {0};
    bleu_device_name(name, zero);
    TEST_ASSERT_EQUAL_STRING("ePager-0000", name);
}

static void test_advertising_bytes(void)
{
    cJSON *g = gatt();
    cJSON *adv = cJSON_GetObjectItem(g, "advertising");
    TEST_ASSERT_EQUAL_STRING(PROTO_ADV_NAME_PREFIX, cJSON_GetObjectItem(adv, "name_prefix")->valuestring);

    uint8_t ancs[16], svc[16];
    uuid_le(cJSON_GetObjectItem(cJSON_GetObjectItem(g, "ancs"), "service")->valuestring, ancs);
    uuid_le(cJSON_GetObjectItem(cJSON_GetObjectItem(g, "service"), "uuid")->valuestring, svc);

    uint8_t a[BLEU_ADV_MAX];
    TEST_ASSERT_EQUAL_UINT(29, bleu_adv_data(a, sizeof(a)));
    const uint8_t head[] = {2, 0x01, 0x06, 17, 0x15};
    TEST_ASSERT_EQUAL_MEMORY(head, a, sizeof(head));
    TEST_ASSERT_EQUAL_MEMORY(ancs, &a[5], 16); /* solicitation for ANCS */
    const uint8_t tail[] = {7, 0x08, 'e', 'P', 'a', 'g', 'e', 'r'};
    TEST_ASSERT_EQUAL_MEMORY(tail, &a[21], sizeof(tail));

    uint8_t s[BLEU_ADV_MAX];
    TEST_ASSERT_EQUAL_UINT(31, bleu_scan_rsp_data("ePager-CDEF", s, sizeof(s)));
    TEST_ASSERT_EQUAL_UINT8(12, s[0]);
    TEST_ASSERT_EQUAL_UINT8(0x09, s[1]);
    TEST_ASSERT_EQUAL_MEMORY("ePager-CDEF", &s[2], 11);
    TEST_ASSERT_EQUAL_UINT8(17, s[13]);
    TEST_ASSERT_EQUAL_UINT8(0x07, s[14]);
    TEST_ASSERT_EQUAL_MEMORY(svc, &s[15], 16); /* the ePager service */

    uint8_t small[28];
    TEST_ASSERT_EQUAL_UINT(0, bleu_adv_data(small, sizeof(small)));
    TEST_ASSERT_EQUAL_UINT(0, bleu_scan_rsp_data("ePager-CDEF", small, 30));
    cJSON_Delete(g);
}

static void test_generated_uuids_match_gatt_json(void)
{
    cJSON *g = gatt();
    const uint8_t gen[] = {PROTO_UUID_CHR_CONFIG_LE};
    uint8_t want[16];
    cJSON *chr;
    cJSON_ArrayForEach(chr, cJSON_GetObjectItem(cJSON_GetObjectItem(g, "service"), "characteristics"))
    {
        if (strcmp(cJSON_GetObjectItem(chr, "name")->valuestring, "config") == 0) {
            uuid_le(cJSON_GetObjectItem(chr, "uuid")->valuestring, want);
            TEST_ASSERT_EQUAL_MEMORY(want, gen, 16);
        }
    }
    cJSON_Delete(g);
}

static void test_passkey(void)
{
    uint32_t pk = 7;
    TEST_ASSERT_TRUE(bleu_passkey(0, &pk));
    TEST_ASSERT_EQUAL_UINT32(0, pk);
    TEST_ASSERT_TRUE(bleu_passkey(123456, &pk));
    TEST_ASSERT_EQUAL_UINT32(123456, pk);
    TEST_ASSERT_TRUE(bleu_passkey(4293999999u, &pk));
    TEST_ASSERT_EQUAL_UINT32(999999, pk);
    pk = 5;
    TEST_ASSERT_FALSE(bleu_passkey(4294000000u, &pk)); /* would favour the low numbers */
    TEST_ASSERT_FALSE(bleu_passkey(0xFFFFFFFFu, &pk));
    TEST_ASSERT_EQUAL_UINT32(5, pk);
    /* Every value it returns has at most six digits, and the mapping is uniform (a crude check). */
    uint32_t seen[10] = {0};
    for (uint32_t r = 0; r < 4294000000u - 1; r += 4294000000u / 1000 + 1) {
        TEST_ASSERT_TRUE(bleu_passkey(r, &pk));
        TEST_ASSERT_TRUE(pk <= 999999);
        seen[pk / 100000]++;
    }
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(seen[i] > 0);
    }
}

static void test_pairing_policy(void)
{
    TEST_ASSERT_TRUE(bleu_pairing_allowed(true, false));  /* pairing mode: a new phone may bond */
    TEST_ASSERT_TRUE(bleu_pairing_allowed(true, true));
    TEST_ASSERT_TRUE(bleu_pairing_allowed(false, true));  /* a bonded phone encrypts again */
    TEST_ASSERT_FALSE(bleu_pairing_allowed(false, false)); /* a stranger: refused */
    TEST_ASSERT_TRUE(bleu_initiate_security(true, false)); /* ask a new phone to pair */
    TEST_ASSERT_FALSE(bleu_initiate_security(true, true)); /* old phone may request repeat pairing only */
    TEST_ASSERT_TRUE(bleu_initiate_security(false, true));
    TEST_ASSERT_FALSE(bleu_initiate_security(false, false));
}

static void test_unsecured_connections_are_dropped(void)
{
    TEST_ASSERT_FALSE(bleu_drop_unsecured(0, false, false));
    TEST_ASSERT_FALSE(bleu_drop_unsecured(BLEU_UNSECURED_MS - 1, false, false));
    TEST_ASSERT_TRUE(bleu_drop_unsecured(BLEU_UNSECURED_MS, false, false));
    TEST_ASSERT_FALSE(bleu_drop_unsecured(BLEU_UNSECURED_MS, true, false)); /* someone is typing a code */
    TEST_ASSERT_TRUE(bleu_drop_unsecured(BLEU_UNSECURED_PAIRING_MS, true, false));
    TEST_ASSERT_FALSE(bleu_drop_unsecured(BLEU_BONDED_PAIRING_GRACE_MS - 1, true, true));
    TEST_ASSERT_TRUE(bleu_drop_unsecured(BLEU_BONDED_PAIRING_GRACE_MS, true, true));
}

static void test_advertising_intervals_are_apples(void)
{
    /* In 0.625 ms units: 20 ms and 1022.5 ms are on Apple's list and must be used exactly. */
    TEST_ASSERT_EQUAL_UINT16(32, BLEU_ADV_FAST_UNITS);
    TEST_ASSERT_EQUAL_UINT16(1636, BLEU_ADV_SLOW_UNITS);
    TEST_ASSERT_EQUAL_UINT16(32, bleu_adv_interval(false, 0));
    TEST_ASSERT_EQUAL_UINT16(32, bleu_adv_interval(false, BLEU_ADV_FAST_MS - 1));
    TEST_ASSERT_EQUAL_UINT16(1636, bleu_adv_interval(false, BLEU_ADV_FAST_MS));
    TEST_ASSERT_EQUAL_UINT16(32, bleu_adv_interval(true, 10 * BLEU_ADV_FAST_MS)); /* pairing mode stays fast */
}

static void test_relaxed_parameters_can_be_overridden_for_tests(void)
{
    bleu_conn_params_t def, p;
    bleu_conn_params(BLEU_CONN_RELAXED, &def);
    const bleu_conn_params_t slow = {96, 120, 4, 600}; /* 120-150 ms, latency 4, 6 s: Apple's rules hold */
    TEST_ASSERT_TRUE(bleu_set_relaxed_override(&slow));
    bleu_conn_params(BLEU_CONN_RELAXED, &p);
    TEST_ASSERT_EQUAL_UINT16(96, p.itvl_min);
    TEST_ASSERT_EQUAL_UINT16(4, p.latency);
    bleu_conn_params(BLEU_CONN_FAST, &p); /* the fast parameters are not touched */
    TEST_ASSERT_EQUAL_UINT16(12, p.itvl_min);
    const bleu_conn_params_t bad = {96, 120, 20, 600}; /* 150 ms x 21 = 3.15 s */
    TEST_ASSERT_FALSE(bleu_set_relaxed_override(&bad));
    bleu_conn_params(BLEU_CONN_RELAXED, &p); /* a refused request leaves the last one */
    TEST_ASSERT_EQUAL_UINT16(96, p.itvl_min);
    TEST_ASSERT_TRUE(bleu_set_relaxed_override(NULL));
    bleu_conn_params(BLEU_CONN_RELAXED, &p);
    TEST_ASSERT_EQUAL_UINT16(def.itvl_min, p.itvl_min);
    TEST_ASSERT_EQUAL_UINT16(def.latency, p.latency);
}

static void test_connection_parameters_follow_apples_rules(void)
{
    bleu_conn_params_t p;
    bleu_conn_params(BLEU_CONN_FAST, &p);
    TEST_ASSERT_TRUE(bleu_conn_params_apple_ok(&p));
    bleu_conn_params(BLEU_CONN_RELAXED, &p);
    TEST_ASSERT_TRUE(bleu_conn_params_apple_ok(&p));

    /* the checker itself: each rule can fail on its own */
    const bleu_conn_params_t good = {12, 24, 0, 400};
    bleu_conn_params_t b = good;
    b.itvl_min = 6; /* 7.5 ms */
    TEST_ASSERT_FALSE(bleu_conn_params_apple_ok(&b));
    b = good;
    b.itvl_min = 16; /* 20 ms: not a multiple of 15 ms */
    b.itvl_max = 40;
    TEST_ASSERT_FALSE(bleu_conn_params_apple_ok(&b));
    b = good;
    b.itvl_max = 12; /* max - min < 15 ms, and not both 15 ms? (12 units = 15 ms: both 15 ms is allowed) */
    TEST_ASSERT_TRUE(bleu_conn_params_apple_ok(&b));
    b.itvl_max = 20; /* 25 ms: only 10 ms above min */
    TEST_ASSERT_FALSE(bleu_conn_params_apple_ok(&b));
    b = good;
    b.latency = 31;
    TEST_ASSERT_FALSE(bleu_conn_params_apple_ok(&b));
    b = good;
    b.itvl_max = 96;
    b.latency = 20; /* 120 ms * 21 = 2.5 s > 2 s */
    b.timeout = 600;
    TEST_ASSERT_FALSE(bleu_conn_params_apple_ok(&b));
    b = good;
    b.timeout = 199; /* under 2 s */
    TEST_ASSERT_FALSE(bleu_conn_params_apple_ok(&b));
    b.timeout = 601; /* over 6 s */
    TEST_ASSERT_FALSE(bleu_conn_params_apple_ok(&b));
    b = good;
    b.itvl_max = 96;
    b.latency = 2;
    b.timeout = 200; /* 120 ms * 3 * 3 = 1.08 s < 2 s timeout: fine */
    TEST_ASSERT_TRUE(bleu_conn_params_apple_ok(&b));
    b.itvl_max = 120; /* 150 ms; latency 12: 1.95 s, x3 = 5.85 s: not under a 4 s timeout */
    b.latency = 12;
    b.timeout = 400;
    TEST_ASSERT_FALSE(bleu_conn_params_apple_ok(&b));
}

static void test_connection_policy(void)
{
    bleu_conn_policy_t p = {0};
    bleu_conn_mode_t m;
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 0, &m)); /* not connected: nothing to ask */

    bleu_policy_connect(&p, 1000);
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 1000, &m)); /* settle first: the central may update the link itself */
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 1000 + BLEU_SETTLE_MS, &m));
    TEST_ASSERT_TRUE(bleu_policy_step(&p, 1001 + BLEU_SETTLE_MS, &m)); /* then the fast parameters */
    TEST_ASSERT_EQUAL_INT(BLEU_CONN_FAST, m);
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 2000 + BLEU_SETTLE_MS, &m));
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 1000 + BLEU_FAST_AFTER_CONNECT_MS, &m)); /* until the window ends */
    TEST_ASSERT_TRUE(bleu_policy_step(&p, 1001 + BLEU_FAST_AFTER_CONNECT_MS, &m));
    TEST_ASSERT_EQUAL_INT(BLEU_CONN_RELAXED, m);

    bleu_policy_activity(&p, 50000); /* a transfer write: fast again, and it stays while writes keep coming */
    TEST_ASSERT_TRUE(bleu_policy_step(&p, 50000, &m));
    TEST_ASSERT_EQUAL_INT(BLEU_CONN_FAST, m);
    bleu_policy_activity(&p, 53000);
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 57999, &m));
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 58000, &m));
    TEST_ASSERT_TRUE(bleu_policy_step(&p, 58001, &m));
    TEST_ASSERT_EQUAL_INT(BLEU_CONN_RELAXED, m);
    bleu_policy_activity(&p, 58002); /* earlier activity never shortens the window */
    bleu_policy_activity(&p, 58000);
    TEST_ASSERT_TRUE(bleu_policy_step(&p, 58003, &m));

    bleu_policy_disconnect(&p);
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 999999, &m));

    /* the 32-bit millisecond counter wraps after 49 days */
    bleu_policy_connect(&p, 0xFFFFFFF0u);
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 0xFFFFFFF0u + BLEU_SETTLE_MS, &m));
    TEST_ASSERT_TRUE(bleu_policy_step(&p, 0xFFFFFFF0u + BLEU_SETTLE_MS + 1, &m)); /* across the wrap */
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 100, &m)); /* nothing more until the fast window ends */
    TEST_ASSERT_TRUE(bleu_policy_step(&p, 0xFFFFFFF0u + BLEU_FAST_AFTER_CONNECT_MS + 1, &m));
}

/* The iPad's own connection update arrived while ours was pending and crashed the controller (2026-10-14):
 * after any update the link must be quiet before a new request goes out. */
static void test_nothing_is_requested_around_an_update(void)
{
    bleu_conn_policy_t p = {0};
    bleu_conn_mode_t m;
    bleu_policy_connect(&p, 0);
    bleu_policy_updated(&p, 3000); /* the central updates the link 3 s in */
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 4001, &m)); /* the settle time is over, but the link was just touched */
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 3000 + BLEU_QUIET_MS, &m));
    TEST_ASSERT_TRUE(bleu_policy_step(&p, 3001 + BLEU_QUIET_MS, &m));
    TEST_ASSERT_EQUAL_INT(BLEU_CONN_FAST, m);
    bleu_policy_updated(&p, 4600); /* the answer to our own request: quiet again before the next one */
    bleu_policy_activity(&p, 4600);
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 4600 + BLEU_QUIET_MS, &m));
    bleu_policy_updated(&p, 3000); /* an older update never shortens the wait */
    TEST_ASSERT_FALSE(bleu_policy_step(&p, 4600 + BLEU_QUIET_MS, &m));
}

static void test_fifo(void)
{
    uint8_t store[64], out[64], tag;
    uint16_t len;
    bleu_fifo_t f;
    bleu_fifo_init(&f, store, sizeof(store));
    TEST_ASSERT_TRUE(bleu_fifo_empty(&f));
    TEST_ASSERT_FALSE(bleu_fifo_peek(&f, &tag, out, sizeof(out), &len));

    const uint8_t a[] = {1, 2, 3}, b[] = {9};
    TEST_ASSERT_TRUE(bleu_fifo_push(&f, 7, a, sizeof(a)));
    TEST_ASSERT_TRUE(bleu_fifo_push(&f, 8, b, sizeof(b)));
    TEST_ASSERT_TRUE(bleu_fifo_push(&f, 9, NULL, 0)); /* an empty record is fine */
    TEST_ASSERT_TRUE(bleu_fifo_peek(&f, &tag, out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_UINT8(7, tag);
    TEST_ASSERT_EQUAL_UINT16(3, len);
    TEST_ASSERT_EQUAL_MEMORY(a, out, 3);
    TEST_ASSERT_FALSE(bleu_fifo_peek(&f, &tag, out, 2, &len)); /* the buffer is too small: nothing is lost */
    TEST_ASSERT_TRUE(bleu_fifo_peek(&f, &tag, out, 3, &len));
    bleu_fifo_drop(&f);
    TEST_ASSERT_TRUE(bleu_fifo_peek(&f, &tag, out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_UINT8(8, tag);
    bleu_fifo_drop(&f);
    TEST_ASSERT_TRUE(bleu_fifo_peek(&f, &tag, out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_UINT8(9, tag);
    TEST_ASSERT_EQUAL_UINT16(0, len);
    bleu_fifo_drop(&f);
    TEST_ASSERT_TRUE(bleu_fifo_empty(&f));
    bleu_fifo_drop(&f); /* dropping from an empty FIFO is harmless */
    TEST_ASSERT_TRUE(bleu_fifo_empty(&f));
}

static void test_fifo_full_and_wrap(void)
{
    uint8_t store[40], out[40], tag;
    uint16_t len;
    bleu_fifo_t f;
    bleu_fifo_init(&f, store, sizeof(store));
    uint8_t rec[20];
    /* 40 bytes: one 23-byte record fits, a second does not */
    memset(rec, 0xA5, sizeof(rec));
    TEST_ASSERT_TRUE(bleu_fifo_push(&f, 1, rec, sizeof(rec)));
    TEST_ASSERT_FALSE(bleu_fifo_push(&f, 2, rec, sizeof(rec)));
    TEST_ASSERT_EQUAL_UINT32(1, f.drops);
    TEST_ASSERT_FALSE(bleu_fifo_push(&f, 2, rec, BLEU_FIFO_REC_MAX + 1)); /* over the record limit */
    bleu_fifo_drop(&f);

    /* wrap around the end of the buffer many times, with records of every size */
    unsigned seq = 0, expect = 0;
    for (int round = 0; round < 2000; round++) {
        const uint16_t n = (uint16_t)(seq * 7 % 21);
        for (uint16_t i = 0; i < n; i++) {
            rec[i] = (uint8_t)(seq + i);
        }
        if (bleu_fifo_push(&f, (uint8_t)seq, rec, n)) {
            seq++;
        }
        if (round % 3 == 2 || f.used > 25) {
            while (bleu_fifo_peek(&f, &tag, out, sizeof(out), &len)) {
                TEST_ASSERT_EQUAL_UINT8((uint8_t)expect, tag);
                TEST_ASSERT_EQUAL_UINT16(expect * 7 % 21, len);
                for (uint16_t i = 0; i < len; i++) {
                    TEST_ASSERT_EQUAL_UINT8((uint8_t)(expect + i), out[i]);
                }
                bleu_fifo_drop(&f);
                expect++;
            }
        }
    }
    TEST_ASSERT_TRUE(expect > 500);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_name);
    RUN_TEST(test_advertising_bytes);
    RUN_TEST(test_generated_uuids_match_gatt_json);
    RUN_TEST(test_passkey);
    RUN_TEST(test_pairing_policy);
    RUN_TEST(test_unsecured_connections_are_dropped);
    RUN_TEST(test_advertising_intervals_are_apples);
    RUN_TEST(test_relaxed_parameters_can_be_overridden_for_tests);
    RUN_TEST(test_connection_parameters_follow_apples_rules);
    RUN_TEST(test_connection_policy);
    RUN_TEST(test_nothing_is_requested_around_an_update);
    RUN_TEST(test_fifo);
    RUN_TEST(test_fifo_full_and_wrap);
    return UNITY_END();
}
