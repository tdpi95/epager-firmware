/* Unit tests for behaviour not covered by the shared vectors (C API edge cases). */
#include <string.h>

#include "test_support.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_registry_tables_consistent(void)
{
    for (size_t i = 0; i < proto_messages_count; i++) {
        const proto_msg_desc_t *m = &proto_messages[i];
        TEST_ASSERT_LESS_OR_EQUAL(32, m->body.n_fields);
        for (unsigned f = 1; f < m->body.n_fields; f++) {
            TEST_ASSERT_TRUE_MESSAGE(m->body.fields[f - 1].tag < m->body.fields[f].tag, m->body.name);
        }
        if (m->kind == PROTO_KIND_REQUEST && m->id != PROTO_MSG_XFER_CHUNK) {
            const proto_msg_desc_t *r = proto_msg_desc(proto_response_type(m->id));
            TEST_ASSERT_NOT_NULL_MESSAGE(r, m->body.name);
            TEST_ASSERT_EQUAL_INT(PROTO_KIND_RESPONSE, r->kind);
        }
    }
    for (int s = 0; s < PROTO_STATUS_COUNT; s++) {
        TEST_ASSERT_NOT_NULL(proto_status_names[s]);
    }
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", proto_status_name((proto_status_t)200));
}

static void test_config_defaults_are_valid(void)
{
    for (size_t i = 0; i < proto_cfg_keys_count; i++) {
        const proto_cfg_key_desc_t *k = &proto_cfg_keys[i];
        uint8_t v[64];
        size_t n;
        switch (k->type) {
        case PROTO_FT_STR: n = strlen(k->default_str); memcpy(v, k->default_str, n); break;
        case PROTO_FT_BYTES: n = 0; break; /* the default of a bytes key is empty */
        case PROTO_FT_U16: case PROTO_FT_I16: n = 2; proto_wr16(v, (uint16_t)k->default_int); break;
        case PROTO_FT_U32: case PROTO_FT_I32: n = 4; proto_wr32(v, (uint32_t)k->default_int); break;
        default: n = 1; v[0] = (uint8_t)k->default_int; break;
        }
        uint8_t variant = PROTO_VARIANT_PAPER;
        if (!proto_cfg_key_applies(k, variant)) {
            variant = PROTO_VARIANT_LCD;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(PROTO_OK, proto_cfg_validate(k->id, v, n, variant), k->name);
    }
}

static void test_writer_overflow_is_sticky(void)
{
    uint8_t buf[8];
    proto_writer_t w;
    proto_writer_init(&w, buf, sizeof(buf));
    proto_w_header(&w, PROTO_MSG_PING, 1);
    proto_w_u8(&w, 1, 2);  /* 4 + 4 = 8: fits exactly */
    TEST_ASSERT_TRUE(proto_writer_ok(&w));
    proto_w_u8(&w, 2, 3);
    TEST_ASSERT_FALSE(proto_writer_ok(&w));
    TEST_ASSERT_EQUAL_UINT(8, w.len);
}

static void test_writer_struct_nesting(void)
{
    uint8_t buf[64];
    proto_writer_t w;
    proto_writer_init(&w, buf, sizeof(buf));
    proto_w_struct_begin(&w, 3);
    proto_w_u16(&w, 1, 0x1234);
    proto_w_struct_end(&w);
    TEST_ASSERT_TRUE(proto_writer_ok(&w));
    const uint8_t want[] = {3, 5, 0, 1, 2, 0, 0x34, 0x12};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, buf, sizeof(want));

    proto_writer_init(&w, buf, sizeof(buf));
    proto_w_struct_end(&w); /* unbalanced */
    TEST_ASSERT_FALSE(proto_writer_ok(&w));
    proto_writer_init(&w, buf, sizeof(buf));
    proto_w_struct_begin(&w, 1); /* left open */
    TEST_ASSERT_FALSE(proto_writer_ok(&w));
    proto_writer_init(&w, buf, sizeof(buf));
    for (int i = 0; i <= PROTO_WRITER_MAX_DEPTH; i++) {
        proto_w_struct_begin(&w, 1);
    }
    TEST_ASSERT_FALSE(proto_writer_ok(&w));
}

static void test_accessors_and_repeated_iteration(void)
{
    uint8_t buf[64];
    proto_writer_t w;
    proto_writer_init(&w, buf, sizeof(buf));
    proto_w_header(&w, PROTO_MSG_CONFIG_GET, 9);
    proto_w_u16(&w, PROTO_TAG_CONFIG_GET_KEY, 1);
    proto_w_u16(&w, PROTO_TAG_CONFIG_GET_KEY, 3);
    proto_w_u16(&w, PROTO_TAG_CONFIG_GET_KEY, 16);
    proto_msg_t m;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, proto_msg_parse(buf, w.len, &m));
    TEST_ASSERT_EQUAL_UINT16(9, m.req_id);
    uint16_t keys[4], k;
    size_t pos = 0, n = 0;
    const uint8_t *v;
    uint16_t vl;
    while (proto_find_next(m.body, m.body_len, PROTO_TAG_CONFIG_GET_KEY, &pos, &v, &vl)) {
        keys[n++] = proto_rd16(v);
    }
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_UINT16(16, keys[2]);
    TEST_ASSERT_TRUE(proto_get_u16(m.body, m.body_len, PROTO_TAG_CONFIG_GET_KEY, &k));
    TEST_ASSERT_EQUAL_UINT16(1, k);
    uint32_t u;
    TEST_ASSERT_FALSE(proto_get_u32(m.body, m.body_len, PROTO_TAG_CONFIG_GET_KEY, &u)); /* wrong width */
    TEST_ASSERT_FALSE(proto_get_u8(m.body, m.body_len, 99, (uint8_t *)&u));
}

static void test_parse_reports_header_on_error(void)
{
    const uint8_t bad[] = {0, 0x5e, 0x34, 0x12};
    proto_msg_t m;
    TEST_ASSERT_EQUAL_INT(PROTO_UNKNOWN_MESSAGE, proto_msg_parse(bad, sizeof(bad), &m));
    TEST_ASSERT_TRUE(m.header_valid);
    TEST_ASSERT_EQUAL_UINT16(0x1234, m.req_id);
    TEST_ASSERT_EQUAL_INT(PROTO_MALFORMED, proto_msg_parse(bad, 3, &m));
    TEST_ASSERT_FALSE(m.header_valid);
}

static void test_utf8_roundtrip_all_scalars(void)
{
    for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            continue;
        }
        uint8_t b[4];
        size_t n = proto_utf8_encode(cp, b);
        uint32_t back;
        TEST_ASSERT_EQUAL_UINT(n, proto_utf8_decode(b, n, &back));
        TEST_ASSERT_EQUAL_HEX32(cp, back);
        TEST_ASSERT_TRUE(proto_utf8_valid(b, n));
    }
    const uint8_t fffd[] = {0xEF, 0xBF, 0xBD};
    TEST_ASSERT_TRUE(proto_utf8_valid(fffd, 3)); /* a literal U+FFFD is valid */
}

static bool only_ascii(uint32_t cp, void *ctx)
{
    (void)ctx;
    return cp >= 0x20 && cp < 0x7F;
}

static void test_text_custom_predicate_and_small_dst(void)
{
    const char *in = "Việt Nam 123";
    uint8_t out[64];
    size_t n = proto_text_normalize((const uint8_t *)in, strlen(in), out, sizeof(out), PROTO_TEXT_NO_BUDGET,
                                    only_ascii, NULL);
    TEST_ASSERT_EQUAL_STRING_LEN("Vi\xE2\x96\xA1t Nam 123", (const char *)out, n);

    /* dst_cap smaller than the output: truncated like a budget, ellipsis included */
    n = proto_text_normalize((const uint8_t *)"Hello, world!", 13, out, 8, PROTO_TEXT_NO_BUDGET, NULL, NULL);
    TEST_ASSERT_EQUAL_STRING_LEN("Hello\xE2\x80\xA6", (const char *)out, n);
    TEST_ASSERT_TRUE(proto_utf8_valid(out, n));
    n = proto_text_normalize((const uint8_t *)"abc", 3, out, 0, PROTO_TEXT_NO_BUDGET, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT(0, n);
}

static void test_seg_tx_edges(void)
{
    uint8_t msg[10] = {0}, seg[20];
    proto_seg_tx_t tx;
    proto_seg_tx_init(&tx, msg, sizeof(msg), 1); /* too small */
    TEST_ASSERT_EQUAL_UINT(0, proto_seg_tx_next(&tx, seg, sizeof(seg)));
    proto_seg_tx_init(&tx, msg, sizeof(msg), 20);
    TEST_ASSERT_EQUAL_UINT(0, proto_seg_tx_next(&tx, seg, 5)); /* cap < max_segment */
    TEST_ASSERT_EQUAL_UINT(11, proto_seg_tx_next(&tx, seg, sizeof(seg)));
    TEST_ASSERT_EQUAL_HEX8(0xC0, seg[0]);
    TEST_ASSERT_EQUAL_UINT(0, proto_seg_tx_next(&tx, seg, sizeof(seg)));
}

static void test_seg_tx_empty_message(void)
{
    uint8_t seg[4];
    proto_seg_tx_t tx;
    proto_seg_tx_init(&tx, NULL, 0, 4);
    TEST_ASSERT_EQUAL_UINT(1, proto_seg_tx_next(&tx, seg, sizeof(seg)));
    TEST_ASSERT_EQUAL_HEX8(0xC0, seg[0]);
    TEST_ASSERT_EQUAL_UINT(0, proto_seg_tx_next(&tx, seg, sizeof(seg)));
}

static void test_ancs_builders_bounds(void)
{
    uint8_t out[8];
    proto_ancs_attr_req_t attrs[] = {{PROTO_ANCS_ATTR_TITLE, 64}, {PROTO_ANCS_ATTR_DATE, 0}};
    TEST_ASSERT_EQUAL_UINT(0, proto_ancs_build_get_notif_attrs(1, attrs, 2, out, 8)); /* needs 9 */
    TEST_ASSERT_EQUAL_UINT(0, proto_ancs_build_get_app_attrs((const uint8_t *)"a\0b", 3, (const uint8_t *)"\0", 1,
                                                             out, sizeof(out))); /* NUL inside app id */
    TEST_ASSERT_EQUAL_UINT(0, proto_ancs_build_perform_action(1, 0, out, 5));

    uint8_t buf[32];
    proto_ancs_ds_t ds;
    const uint8_t ids[] = {PROTO_ANCS_ATTR_APP_ID};
    proto_ancs_ds_init_notif(&ds, 7, ids, 1, buf, sizeof(buf));
    const uint8_t *v;
    uint16_t vl;
    TEST_ASSERT_FALSE(proto_ancs_ds_attr(&ds, PROTO_ANCS_ATTR_APP_ID, &v, &vl)); /* not complete yet */
}

static void sha_hex(const void *d, size_t n, char out[65])
{
    uint8_t h[PROTO_SHA256_LEN];
    proto_sha256(d, n, h);
    hex_encode(h, sizeof(h), out);
}

static void test_sha256_known_answers(void)
{
    /* FIPS 180-4 / NIST CAVP examples */
    char hex[65];
    sha_hex("", 0, hex);
    TEST_ASSERT_EQUAL_STRING("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hex);
    sha_hex("abc", 3, hex);
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex);
    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha_hex(two_blocks, strlen(two_blocks), hex);
    TEST_ASSERT_EQUAL_STRING("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", hex);

    proto_sha256_t s;
    uint8_t a[1000], h[PROTO_SHA256_LEN];
    memset(a, 'a', sizeof(a));
    proto_sha256_init(&s);
    for (int i = 0; i < 1000; i++) {
        proto_sha256_update(&s, a, sizeof(a));
    }
    proto_sha256_final(&s, h);
    hex_encode(h, sizeof(h), hex);
    TEST_ASSERT_EQUAL_STRING("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", hex);
}

static void test_sha256_incremental_matches_one_shot(void)
{
    uint8_t data[200], whole[PROTO_SHA256_LEN], part[PROTO_SHA256_LEN];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 7 + 3);
    }
    for (size_t len = 0; len <= sizeof(data); len += 13) {
        proto_sha256(data, len, whole);
        for (size_t cut = 0; cut <= len; cut++) {
            proto_sha256_t s;
            proto_sha256_init(&s);
            proto_sha256_update(&s, data, cut);
            proto_sha256_update(&s, data + cut, len - cut);
            proto_sha256_final(&s, part);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(whole, part, PROTO_SHA256_LEN);
        }
    }
}

static void test_config_since_versions(void)
{
    for (size_t i = 0; i < proto_cfg_keys_count; i++) {
        TEST_ASSERT_NOT_EQUAL(0, proto_cfg_keys[i].since);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(PROTO_CONTRACT_VERSION_PACKED, proto_cfg_keys[i].since);
    }
    TEST_ASSERT_EQUAL_UINT(PROTO_CFG_KEY_COUNT, proto_cfg_keys_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_registry_tables_consistent);
    RUN_TEST(test_config_defaults_are_valid);
    RUN_TEST(test_writer_overflow_is_sticky);
    RUN_TEST(test_writer_struct_nesting);
    RUN_TEST(test_accessors_and_repeated_iteration);
    RUN_TEST(test_parse_reports_header_on_error);
    RUN_TEST(test_utf8_roundtrip_all_scalars);
    RUN_TEST(test_text_custom_predicate_and_small_dst);
    RUN_TEST(test_seg_tx_edges);
    RUN_TEST(test_seg_tx_empty_message);
    RUN_TEST(test_ancs_builders_bounds);
    RUN_TEST(test_sha256_known_answers);
    RUN_TEST(test_sha256_incremental_matches_one_shot);
    RUN_TEST(test_config_since_versions);
    return UNITY_END();
}
