/* Clock faces: the C parser, option merging and evaluator against contracts/test-vectors/clock_faces.json, edge
 * cases of their own, and a fuzz run (iterations: EPAGER_FUZZ_ITERS, default 20000; the sanitizers watch). */
#include <stdlib.h>
#include <string.h>

#include "epager/clock_face.h"
#include "test_support.h"
#include "unity.h"

#define FILE_MAX 32768u

static cf_face_t face; /* large: not on the stack */
static const cJSON *g_faces;

void setUp(void) { g_case_failures = 0; }
void tearDown(void) {}

#define FINISH_CASES() TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_case_failures, "failing vector cases (see stderr)")

static const cf_limits_t PAPER = {200, 200, 65536, 1u << PROTO_ASSET_FORMAT_MONO1, false};
static const cf_limits_t LCD = {296, 240, 1u << 20,
                                (1u << PROTO_ASSET_FORMAT_MONO1) | (1u << PROTO_ASSET_FORMAT_RGB565) |
                                    (1u << PROTO_ASSET_FORMAT_RGB565A8),
                                true};

static const cf_limits_t *limits_of(const char *board) { return strcmp(board, "lcd") == 0 ? &LCD : &PAPER; }

/* The face file of a vector: its own file_hex, or the named face's. Returns the length, SIZE_MAX if missing. */
static size_t file_of(const cJSON *c, uint8_t *buf, size_t cap)
{
    const char *hex = jstr(c, "file_hex");
    if (!hex) {
        const char *name = jstr(c, "face");
        const cJSON *f = name ? cJSON_GetObjectItemCaseSensitive(g_faces, name) : NULL;
        hex = f ? jstr(f, "file_hex") : NULL;
    }
    return hex ? hex_decode(hex, buf, cap) : SIZE_MAX;
}

static const cJSON *face_json_of(const cJSON *doc, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(doc, "faces"), name);
}

static void test_parse_vectors(void)
{
    cJSON *doc = load_vectors("clock_faces.json");
    g_faces = cJSON_GetObjectItemCaseSensitive(doc, "faces");
    static uint8_t buf[FILE_MAX];
    int n = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(doc, "parse")) {
        const char *name = jstr(c, "name");
        cf_limits_t lim = *limits_of(jstr(c, "board"));
        if (jnum(c, "max_file", 0) > 0) {
            lim.max_file = (uint32_t)jnum(c, "max_file", 0);
        }
        const size_t len = file_of(c, buf, sizeof(buf));
        if (len == SIZE_MAX) {
            CASE_FAIL(name, "file does not decode");
            continue;
        }
        const char *why = "";
        const proto_status_t got = cf_parse(buf, len, &lim, jstr(c, "id"), PROTO_CONTRACT_VERSION_PACKED, &face, &why);
        const proto_status_t want = status_by_name(jstr(c, "expect"));
        n++;
        if (got != want) {
            CASE_FAIL(name, "status %s (%s), want %s", proto_status_name(got), why, proto_status_name(want));
            continue;
        }
        const cJSON *sum = cJSON_GetObjectItemCaseSensitive(c, "summary");
        if (got != PROTO_OK || !sum) {
            continue;
        }
        if (strcmp(face.id, jstr(sum, "id")) != 0 || strcmp(face.name[0], jstr(sum, "name")) != 0 ||
            strcmp(face.version, jstr(sum, "version")) != 0 || face.n_images != (int)jnum(sum, "images", -1) ||
            face.n_options != (int)jnum(sum, "options", -1) || face.n_elements != (int)jnum(sum, "elements", -1) ||
            cf_draws_source(&face, PROTO_FACE_SOURCE_UPDATE_AVAILABLE) !=
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sum, "draws_update_available"))) {
            CASE_FAIL(name, "summary differs (id %s, %u images, %u options, %u elements)", face.id, face.n_images,
                      face.n_options, face.n_elements);
        }
    }
    TEST_ASSERT_GREATER_THAN(40, n);
    FINISH_CASES();
    cJSON_Delete(doc);
}

static bool parse_named(cJSON *doc, const char *name, cf_face_t *out, uint8_t *buf, size_t cap)
{
    const cJSON *f = face_json_of(doc, name);
    const size_t len = hex_decode(jstr(f, "file_hex"), buf, cap);
    return cf_parse(buf, len, limits_of(jstr(f, "board")), jstr(f, "id"), PROTO_CONTRACT_VERSION_PACKED, out, NULL) ==
           PROTO_OK;
}

static void test_option_vectors(void)
{
    cJSON *doc = load_vectors("clock_faces.json");
    static uint8_t fbuf[FILE_MAX];
    TEST_ASSERT_TRUE(parse_named(doc, "paper_full", &face, fbuf, sizeof(fbuf)));
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(doc, "options")) {
        const char *name = jstr(c, "name");
        uint8_t pairs[64], values[CF_MAX_OPTIONS], packed[2 * CF_MAX_OPTIONS];
        const size_t np = hex_decode(jstr(c, "pairs_hex"), pairs, sizeof(pairs));
        cf_opts_resolve(&face, pairs, np, values);
        const cJSON *want = cJSON_GetObjectItemCaseSensitive(c, "values");
        TEST_ASSERT_EQUAL_INT(face.n_options, cJSON_GetArraySize(want));
        for (int i = 0; i < cJSON_GetArraySize(want); i++) {
            if (values[i] != (uint8_t)cJSON_GetArrayItem(want, i)->valuedouble) {
                CASE_FAIL(name, "option %d is %u", i, values[i]);
            }
        }
        const size_t pn = cf_opts_pack(&face, values, packed, sizeof(packed));
        char hex[2 * sizeof(packed) + 1];
        hex_encode(packed, pn, hex);
        if (strcmp(hex, jstr(c, "packed_hex")) != 0) {
            CASE_FAIL(name, "packed %s, want %s", hex, jstr(c, "packed_hex"));
        }
    }
    FINISH_CASES();
    cJSON_Delete(doc);
}

static const char *const LANG[] = {"en", "vi"};

static void fill_data(const cJSON *j, cf_data_t *d)
{
    memset(d, 0, sizeof(*d));
    d->time_known = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "time_known"));
    d->hour = (uint8_t)jnum(j, "hour", 0);
    d->minute = (uint8_t)jnum(j, "minute", 0);
    d->day = (uint8_t)jnum(j, "day", 1);
    d->month = (uint8_t)jnum(j, "month", 1);
    d->year = (uint16_t)jnum(j, "year", 2000);
    d->wday = (uint8_t)jnum(j, "wday", 0);
    d->clock_24h = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "clock_24h"));
    d->language = strcmp(jstr(j, "language"), LANG[1]) == 0 ? PROTO_LANGUAGE_VI : PROTO_LANGUAGE_EN;
    d->date = jstr(j, "date");
    d->fahrenheit = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "fahrenheit"));
    const cJSON *wx = cJSON_GetObjectItemCaseSensitive(j, "wx");
    d->has_wx_temp = cJSON_GetObjectItemCaseSensitive(wx, "temp_c10") != NULL;
    d->has_wx_high = cJSON_GetObjectItemCaseSensitive(wx, "high_c10") != NULL;
    d->has_wx_low = cJSON_GetObjectItemCaseSensitive(wx, "low_c10") != NULL;
    d->has_wx_rain = cJSON_GetObjectItemCaseSensitive(wx, "rain") != NULL;
    d->has_wx_cond = cJSON_GetObjectItemCaseSensitive(wx, "cond") != NULL;
    d->wx_temp_c10 = (int16_t)jnum(wx, "temp_c10", 0);
    d->wx_high_c10 = (int16_t)jnum(wx, "high_c10", 0);
    d->wx_low_c10 = (int16_t)jnum(wx, "low_c10", 0);
    d->wx_rain = (uint8_t)jnum(wx, "rain", 0);
    d->wx_cond = (uint8_t)jnum(wx, "cond", 0);
    const cJSON *day = cJSON_GetObjectItemCaseSensitive(wx, "day");
    d->wx_day = day ? cJSON_IsTrue(day) : true;
    d->wx_place = jstr(wx, "place");
    const cJSON *room = cJSON_GetObjectItemCaseSensitive(j, "room");
    d->has_room = cJSON_IsObject(room);
    d->room_temp_c100 = (int16_t)jnum(room, "temp_c100", 0);
    d->room_rh_c100 = (uint16_t)jnum(room, "rh_c100", 0);
    d->battery_pct = (uint8_t)jnum(j, "battery", 255);
    d->charging = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "charging"));
    d->ble = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ble"));
    d->wifi = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "wifi"));
    d->notif_count = (uint16_t)jnum(j, "notif", 0);
    d->update_available = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "update"));
}

static void test_eval_vectors(void)
{
    cJSON *doc = load_vectors("clock_faces.json");
    static uint8_t fbuf[FILE_MAX];
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(doc, "eval")) {
        const char *name = jstr(c, "name");
        TEST_ASSERT_TRUE(parse_named(doc, jstr(c, "face"), &face, fbuf, sizeof(fbuf)));
        uint8_t pairs[64], values[CF_MAX_OPTIONS];
        const size_t np = hex_decode(jstr(c, "pairs_hex"), pairs, sizeof(pairs));
        cf_opts_resolve(&face, pairs, np, values);
        cf_data_t d;
        fill_data(cJSON_GetObjectItemCaseSensitive(c, "data"), &d);
        const cJSON *want = cJSON_GetObjectItemCaseSensitive(c, "elements");
        if ((unsigned)cJSON_GetArraySize(want) != face.n_elements) {
            CASE_FAIL(name, "%d expected elements, %u in the face", cJSON_GetArraySize(want), face.n_elements);
            continue;
        }
        for (unsigned i = 0; i < face.n_elements; i++) {
            const cJSON *w = cJSON_GetArrayItem(want, (int)i);
            cf_out_t o;
            cf_eval(&face, values, &d, i, &o);
            if (o.visible != cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(w, "visible"))) {
                CASE_FAIL(name, "element %u: visible %d", i, o.visible);
                continue;
            }
            if (!o.visible) {
                continue;
            }
            const char *text = jstr(w, "text");
            const cJSON *gl = cJSON_GetObjectItemCaseSensitive(w, "glyphs");
            bool ok = (uint16_t)jnum(w, "colour", -1) == o.colour && strcmp(text ? text : "", o.text) == 0 &&
                      (int32_t)jnum(w, "value", 0) == o.value && (uint8_t)jnum(w, "aux", 0) == o.aux &&
                      (gl ? cJSON_GetArraySize(gl) : 0) == o.n_glyph;
            for (int k = 0; ok && gl && k < cJSON_GetArraySize(gl); k++) {
                ok = (uint8_t)cJSON_GetArrayItem(gl, k)->valuedouble == o.glyph[k];
            }
            if (!ok) {
                CASE_FAIL(name, "element %u: text \"%s\" colour %04x value %d aux %u glyphs %u", i, o.text, o.colour,
                          (int)o.value, o.aux, o.n_glyph);
            }
        }
    }
    FINISH_CASES();
    cJSON_Delete(doc);
}

static void test_option_set_and_odd_pairs(void)
{
    cJSON *doc = load_vectors("clock_faces.json");
    static uint8_t fbuf[FILE_MAX];
    TEST_ASSERT_TRUE(parse_named(doc, "paper_full", &face, fbuf, sizeof(fbuf)));
    uint8_t values[CF_MAX_OPTIONS], packed[2 * CF_MAX_OPTIONS];
    cf_opts_resolve(&face, (const uint8_t[]){2, 1}, 2, values);
    TEST_ASSERT_EQUAL_UINT8(1, values[0]); /* the toggle's default */
    TEST_ASSERT_EQUAL_UINT8(1, values[1]);
    TEST_ASSERT_EQUAL_UINT8(1, values[2]); /* the colour option's default */
    /* everything at its default packs to nothing */
    cf_opts_resolve(&face, NULL, 0, values);
    TEST_ASSERT_EQUAL_UINT(0, cf_opts_pack(&face, values, packed, sizeof(packed)));
    /* a cap smaller than the pairs never overruns */
    values[0] = 0;
    values[1] = 1;
    TEST_ASSERT_EQUAL_UINT(2, cf_opts_pack(&face, values, packed, 3));
    cJSON_Delete(doc);
}

/* ---------------------------------------------------------------- fuzz */

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static long iters(void)
{
    const char *e = getenv("EPAGER_FUZZ_ITERS");
    return e ? atol(e) : 20000;
}

static void test_fuzz_parse(void)
{
    cJSON *doc = load_vectors("clock_faces.json");
    static uint8_t seeds[4][FILE_MAX], buf[FILE_MAX + 64];
    size_t seed_len[4], n_seeds = 0;
    const char *names[] = {"paper_full", "lcd_full", "paper_min", "lcd_min"};
    for (size_t i = 0; i < 4; i++) {
        seed_len[i] = hex_decode(jstr(face_json_of(doc, names[i]), "file_hex"), seeds[i], sizeof(seeds[i]));
        n_seeds++;
    }
    long accepted = 0;
    for (long it = 0; it < iters(); it++) {
        const size_t k = rnd() % n_seeds;
        size_t len = seed_len[k];
        memcpy(buf, seeds[k], len);
        const unsigned flips = 1u + (unsigned)(rnd() % 4u);
        for (unsigned f = 0; f < flips; f++) {
            switch (rnd() % 4u) {
            case 0: buf[rnd() % len] = (uint8_t)rnd(); break;
            case 1: buf[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8u)); break;
            case 2: len = 1 + rnd() % len; break; /* truncate */
            default: /* patch a 16-bit length or tag near the start of the description */
                if (len > 40) {
                    buf[20 + rnd() % 20] = (uint8_t)rnd();
                }
                break;
            }
        }
        /* the buffer is exactly `len` bytes for ASan: copy to a tight allocation */
        uint8_t *tight = malloc(len);
        memcpy(tight, buf, len);
        const cf_limits_t *lim = (rnd() & 1) ? &PAPER : &LCD;
        if (cf_parse(tight, len, lim, NULL, PROTO_CONTRACT_VERSION_PACKED, &face, NULL) == PROTO_OK) {
            accepted++;
            /* a face that parsed is safe to evaluate and read back */
            uint8_t values[CF_MAX_OPTIONS];
            cf_opts_resolve(&face, NULL, 0, values);
            cf_data_t d;
            memset(&d, 0, sizeof(d));
            d.time_known = true;
            d.clock_24h = true;
            d.hour = 13;
            d.minute = 7;
            d.day = 4;
            d.month = 11;
            d.year = 2026;
            d.wday = 3;
            d.date = "x";
            d.battery_pct = 50;
            for (unsigned i = 0; i < face.n_elements; i++) {
                cf_out_t o;
                cf_eval(&face, values, &d, i, &o);
            }
            for (unsigned i = 0; i < face.n_images; i++) { /* every image lies inside the file */
                TEST_ASSERT_TRUE(face.image[i].data >= tight && face.image[i].data + face.image[i].len <= tight + len);
            }
        }
        free(tight);
    }
    printf("clock face parse: %ld/%ld mutants still valid\n", accepted, iters());
    cJSON_Delete(doc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_vectors);
    RUN_TEST(test_option_vectors);
    RUN_TEST(test_eval_vectors);
    RUN_TEST(test_option_set_and_odd_pairs);
    RUN_TEST(test_fuzz_parse);
    return UNITY_END();
}
