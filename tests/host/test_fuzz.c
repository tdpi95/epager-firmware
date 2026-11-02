/* Deterministic mutation fuzzing of every parser (run under ASan/UBSan).
 * Iterations per target: EPAGER_FUZZ_ITERS (default 100000). */
#include <stdlib.h>
#include <string.h>

#include "epager/npipe.h"
#include "test_support.h"
#include "unity.h"
#include "xboard.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }

static long iters(void)
{
    const char *e = getenv("EPAGER_FUZZ_ITERS");
    return e ? atol(e) : 100000;
}

void setUp(void) {}
void tearDown(void) {}

#define MAXV 256
static uint8_t *seeds[MAXV];
static size_t seed_len[MAXV];
static size_t n_seeds;

static void load_seeds(void)
{
    cJSON *doc = load_vectors("messages.json");
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(doc, "cases")) {
        if (n_seeds == MAXV) {
            break;
        }
        uint8_t buf[4096];
        size_t n = hex_decode(jstr(c, "hex"), buf, sizeof(buf));
        seeds[n_seeds] = malloc(n ? n : 1);
        memcpy(seeds[n_seeds], buf, n);
        seed_len[n_seeds++] = n;
    }
    cJSON_Delete(doc);
}

/* Mutate into a fresh exact-size heap buffer so ASan sees any overread. */
static uint8_t *mutate(const uint8_t *in, size_t len, size_t *out_len)
{
    uint8_t tmp[4200];
    size_t n = len;
    memcpy(tmp, in, len);
    unsigned ops = 1 + below(4);
    for (unsigned k = 0; k < ops; k++) {
        switch (below(6)) {
        case 0: if (n) tmp[below((uint32_t)n)] ^= (uint8_t)(1u << below(8)); break;
        case 1: if (n) tmp[below((uint32_t)n)] = (uint8_t)rnd(); break;
        case 2: n = below((uint32_t)n + 1); break;
        case 3: { size_t add = below(16); while (add-- && n < sizeof(tmp)) tmp[n++] = (uint8_t)rnd(); } break;
        case 4: if (n > 5) { size_t at = 4 + below((uint32_t)n - 4); tmp[at] = (uint8_t)below(16); } break; /* small tags */
        default: if (n > 6) { size_t at = 5 + below((uint32_t)n - 6); tmp[at] = 0xFF; } break;            /* big lengths */
        }
    }
    uint8_t *out = malloc(n ? n : 1);
    memcpy(out, tmp, n);
    *out_len = n;
    return out;
}

static void test_fuzz_message_parse(void)
{
    load_seeds();
    long ok = 0;
    for (long i = 0; i < iters(); i++) {
        size_t s = below((uint32_t)n_seeds), n;
        uint8_t *m = mutate(seeds[s], seed_len[s], &n);
        proto_msg_t msg;
        proto_status_t st = proto_msg_parse(m, n, &msg);
        TEST_ASSERT_LESS_THAN(PROTO_STATUS_COUNT, st);
        if (st == PROTO_OK) {
            ok++;
            cJSON_Delete(msg_to_json(&msg)); /* walks every field of a validated body */
        }
        free(m);
    }
    printf("message parse: %ld/%ld mutants still valid\n", ok, iters());
}

static void test_fuzz_text(void)
{
    static const uint32_t pool[] = {'a', 'e', ' ', '\n', '\r', '\t', 0x01, 0x7F, 0x85, 0x00AD, 0x0301, 0x0302,
                                    0x0323, 0x031B, 0x0309, 0x00EA, 0x01A1, 0x1EC7, 0x200D, 0xFE0F, 0x1F600,
                                    0x1F3FD, 0x4F60, 0x25A1, 0x2026, 0xFEFF};
    for (long i = 0; i < iters(); i++) {
        uint8_t tmp[256];
        size_t n = 0;
        size_t want = below(60);
        while (n + 4 < sizeof(tmp) && want--) {
            if (below(8) == 0) {
                tmp[n++] = (uint8_t)rnd(); /* raw byte: often invalid UTF-8 */
            } else {
                n += proto_utf8_encode(pool[below(sizeof(pool) / sizeof(pool[0]))], tmp + n);
            }
        }
        uint8_t *in = malloc(n ? n : 1);
        memcpy(in, tmp, n);
        size_t cap = below(80), budget = below(4) ? below(100) : PROTO_TEXT_NO_BUDGET;
        uint8_t *out = malloc(cap ? cap : 1);
        size_t got = proto_text_normalize(in, n, out, cap, budget, NULL, NULL);
        TEST_ASSERT_TRUE(got <= cap && got <= budget);
        TEST_ASSERT_TRUE(proto_utf8_valid(out, got));
        free(in);
        free(out);
    }
}

static void test_fuzz_segments(void)
{
    uint8_t *buf = malloc(64);
    proto_seg_rx_t rx;
    proto_seg_rx_init(&rx, buf, 64);
    for (long i = 0; i < iters(); i++) {
        size_t n = below(24);
        uint8_t *seg = malloc(n ? n : 1);
        for (size_t k = 0; k < n; k++) {
            seg[k] = (uint8_t)rnd();
        }
        if (n && below(2)) {
            seg[0] = (uint8_t)((seg[0] & 0xC0) | below(3)); /* plausible sequence numbers */
        }
        proto_seg_event_t ev[2];
        size_t ne = proto_seg_rx_feed(&rx, seg, n, ev);
        TEST_ASSERT_LESS_OR_EQUAL(2, ne);
        for (size_t k = 0; k < ne; k++) {
            if (ev[k].status == PROTO_OK) {
                TEST_ASSERT_TRUE(ev[k].len > 0 && ev[k].len <= 64);
            }
        }
        free(seg);
    }
    free(buf);
}

static void test_fuzz_ancs(void)
{
    const uint8_t ids[] = {0, 1, 2, 3, 5};
    for (long i = 0; i < iters() / 4; i++) {
        size_t cap = 1 + below(128);
        uint8_t *buf = malloc(cap);
        proto_ancs_ds_t ds;
        if (below(2)) {
            proto_ancs_ds_init_notif(&ds, below(3), ids, 1 + below(5), buf, cap);
        } else {
            proto_ancs_ds_init_app(&ds, (const uint8_t *)"com.a", 5, ids, 1, buf, cap);
        }
        for (int f = 0; f < 6; f++) {
            size_t n = below(20);
            uint8_t *frag = malloc(n ? n : 1);
            for (size_t k = 0; k < n; k++) {
                frag[k] = (uint8_t)(below(3) ? below(6) : rnd());
            }
            bool complete;
            proto_status_t st = proto_ancs_ds_feed(&ds, frag, n, &complete);
            free(frag);
            if (complete) {
                for (size_t a = 0; a < sizeof(ids); a++) {
                    const uint8_t *v;
                    uint16_t vl;
                    if (proto_ancs_ds_attr(&ds, ids[a], &v, &vl)) {
                        TEST_ASSERT_TRUE(v >= buf && v + vl <= buf + cap);
                    }
                }
            }
            if (st != PROTO_OK) {
                break;
            }
        }
        uint8_t date[15];
        for (int k = 0; k < 15; k++) {
            date[k] = (uint8_t)(below(4) ? '0' + below(10) : rnd());
        }
        int64_t t;
        (void)proto_ancs_parse_date(date, 15, &t);
        free(buf);
    }
}

/* ---------------------------------------------------------------- M2: storage records */

/* Every message the code under test sends must itself be valid. */
static void assert_valid_out(const uint8_t *msg, size_t len)
{
    proto_msg_t m;
    TEST_ASSERT_EQUAL_INT_MESSAGE(PROTO_OK, proto_msg_parse(msg, len, &m), "invalid message produced");
}

static uint8_t *copy_kv(const kv_mem_t *kv, const char *key, size_t *len)
{
    uint8_t buf[KV_MEM_MAX_VALUE];
    TEST_ASSERT_EQUAL_INT(KV_OK, kv->iface.get(kv->iface.ctx, key, buf, sizeof(buf), len));
    uint8_t *out = malloc(*len);
    memcpy(out, buf, *len);
    return out;
}

static void cfg_get_all(cfg_store_t *cfg)
{
    uint8_t req[8], rsp[PROTO_LIMIT_MAX_MESSAGE_CONFIG];
    proto_writer_t w;
    proto_msg_t m;
    proto_writer_init(&w, req, sizeof(req));
    proto_w_header(&w, PROTO_MSG_CONFIG_GET, 1);
    proto_msg_parse(req, w.len, &m);
    proto_writer_init(&w, rsp, sizeof(rsp));
    cfg_handle(cfg, &m, &w);
    TEST_ASSERT_TRUE(proto_writer_ok(&w));
    assert_valid_out(rsp, w.len);
}

static void test_fuzz_storage_records(void)
{
    static kv_mem_t kv;
    static cfg_store_t cfg;
    static npipe_t np;
    static npipe_slot_t slots[2];
    static npipe_app_t apps[2];
    kv_mem_init(&kv);
    cfg_params_t cp = {.kv = &kv.iface, .variant = PROTO_VARIANT_PAPER};
    cfg_init(&cfg, &cp);
    uint8_t v[2] = {0x34, 0x01};
    cfg_entry_t e[] = {{PROTO_CFG_QUIET_START_MIN, v, 2}, {PROTO_CFG_UI_THEME_ID, (const uint8_t *)"night", 5}};
    TEST_ASSERT_EQUAL_INT(PROTO_OK, cfg_set(&cfg, e, 2, NULL, NULL));
    npipe_params_t np_p = {.cfg = &cfg, .kv = &kv.iface, .slots = slots, .n_slots = 2, .apps = apps, .n_apps = 2};
    npipe_init(&np, &np_p);
    npipe_android_t a = {.key = {(const uint8_t *)"k", 1}, .app_id = {(const uint8_t *)"com.zing.zalo", 13},
                         .app_name = {(const uint8_t *)"Zalo", 4}};
    uint32_t id;
    bool shown;
    npipe_android_post(&np, &a, &id, &shown);

    xboard_t *b = xboard_from_vectors(NULL);
    uint8_t asset[200];
    memcpy(asset, "ESPA\x01\x03\x04\x00\x00\x00\x00\x00\xb8\x00\x00\x00", 16);
    for (int i = 16; i < 200; i++) {
        asset[i] = (uint8_t)i;
    }
    uint8_t sha[32], req[400], data_msg[128];
    proto_sha256(asset, sizeof(asset), sha);
    proto_writer_t w;
    proto_writer_init(&w, req, sizeof(req));
    proto_w_header(&w, PROTO_MSG_XFER_BEGIN, 1);
    proto_w_u8(&w, PROTO_TAG_XFER_BEGIN_KIND, PROTO_TRANSFER_KIND_ASSET);
    proto_w_str(&w, PROTO_TAG_XFER_BEGIN_ID, "x");
    proto_w_u8(&w, PROTO_TAG_XFER_BEGIN_ASSET_TYPE, 3);
    proto_w_u32(&w, PROTO_TAG_XFER_BEGIN_SIZE, sizeof(asset));
    proto_w_u32(&w, PROTO_TAG_XFER_BEGIN_CRC32, proto_crc32_update(0, asset, sizeof(asset)));
    proto_w_bytes(&w, PROTO_TAG_XFER_BEGIN_SHA256, sha, 32);
    proto_w_str(&w, PROTO_TAG_XFER_BEGIN_VERSION, "1.0");
    size_t begin_len = w.len;
    xboard_request(b, req, begin_len);
    proto_writer_init(&w, data_msg, sizeof(data_msg));
    proto_w_header(&w, PROTO_MSG_XFER_CHUNK, 0);
    proto_w_u8(&w, PROTO_TAG_XFER_CHUNK_HANDLE, 1);
    proto_w_u32(&w, PROTO_TAG_XFER_CHUNK_OFFSET, 0);
    proto_w_bytes(&w, PROTO_TAG_XFER_CHUNK_DATA, asset, 64);
    xboard_request(b, data_msg, w.len);
    xfer_on_disconnect(&b->x);

    struct {
        const char *key;
        uint8_t *rec;
        size_t len;
    } recs[] = {{CFG_KV_KEY, NULL, 0}, {"app000", NULL, 0}, {XFER_JOURNAL_KEY, NULL, 0}, {XFER_OTA_KV_KEY, NULL, 0}};
    recs[0].rec = copy_kv(&kv, CFG_KV_KEY, &recs[0].len);
    recs[1].rec = copy_kv(&kv, "app000", &recs[1].len);
    recs[2].rec = copy_kv(&b->kv, XFER_JOURNAL_KEY, &recs[2].len);
    recs[3].rec = malloc(8);
    memcpy(recs[3].rec, "O\x01\x05" "0.2.0", 8);
    recs[3].len = 8;

    for (long i = 0; i < iters() / 4; i++) {
        size_t s = below(4), n;
        uint8_t *m = mutate(recs[s].rec, recs[s].len, &n);
        if (n > KV_MEM_MAX_VALUE) {
            n = KV_MEM_MAX_VALUE;
        }
        if (s == 0) {
            kv.iface.set(kv.iface.ctx, CFG_KV_KEY, m, n);
            cfg_init(&cfg, &cp);
            cfg_get_all(&cfg);
        } else if (s == 1) {
            kv.iface.set(kv.iface.ctx, below(2) ? "app000" : "app001", m, n);
            npipe_init(&np, &np_p);
            npipe_android_post(&np, &a, &id, &shown);
            for (size_t k = 0; k < 2; k++) {
                if (apps[k].used) {
                    TEST_ASSERT_TRUE(apps[k].id_len >= 1 && apps[k].id_len <= NPIPE_APP_ID_MAX);
                    TEST_ASSERT_TRUE(proto_utf8_valid((const uint8_t *)apps[k].id, apps[k].id_len));
                }
            }
        } else {
            b->kv.iface.set(b->kv.iface.ctx, recs[s].key, m, n);
            xboard_reboot(b);
            if (s == 3) {
                xfer_ota_boot(&b->ota, below(2), 0);
            } else {
                xboard_request(b, req, begin_len);
                TEST_ASSERT_EQUAL_INT(1, b->n_out);
                assert_valid_out(b->out[0], b->out_len[0]);
                xboard_request(b, data_msg, w.len);
                xfer_on_disconnect(&b->x);
            }
        }
        free(m);
    }
    for (size_t s = 0; s < 4; s++) {
        free(recs[s].rec);
    }
    xboard_free(b);
}

/* ---------------------------------------------------------------- M2: request handlers */

static void test_fuzz_handlers(void)
{
    static kv_mem_t kv;
    static cfg_store_t cfg;
    static npipe_t np;
    static npipe_slot_t slots[4];
    static npipe_app_t apps[4];
    kv_mem_init(&kv);
    cfg_params_t cp = {.kv = &kv.iface, .variant = PROTO_VARIANT_PAPER};
    npipe_params_t np_p = {.cfg = &cfg, .kv = &kv.iface, .slots = slots, .n_slots = 4, .apps = apps, .n_apps = 4};
    xboard_t *b = xboard_from_vectors(NULL);
    long handled = 0;
    if (n_seeds == 0) {
        load_seeds();
    }
    for (long i = 0; i < iters(); i++) {
        if (i % 1000 == 0) {
            cfg_init(&cfg, &cp);
            npipe_init(&np, &np_p);
        }
        size_t s = below((uint32_t)n_seeds), n;
        uint8_t *m = mutate(seeds[s], seed_len[s], &n);
        proto_msg_t msg;
        proto_status_t st = proto_msg_parse(m, n, &msg);
        uint8_t rsp[PROTO_LIMIT_MAX_MESSAGE_NOTIFICATION];
        proto_writer_t w;
        proto_writer_init(&w, rsp, sizeof(rsp));
        bool answered = false;
        if (msg.header_valid && msg.desc && msg.desc->channel == PROTO_CHAN_TRANSFER) {
            xboard_request(b, m, n);
            for (int k = 0; k < b->n_out; k++) {
                assert_valid_out(b->out[k], b->out_len[k]);
            }
            handled += b->n_out > 0;
        } else if (st == PROTO_OK) {
            answered = cfg_handle(&cfg, &msg, &w) || npipe_handle(&np, &msg, &w);
        }
        if (answered) {
            handled++;
            TEST_ASSERT_TRUE(proto_writer_ok(&w));
            TEST_ASSERT_LESS_OR_EQUAL(msg.desc->channel == PROTO_CHAN_CONFIG ? PROTO_LIMIT_MAX_MESSAGE_CONFIG
                                                                             : PROTO_LIMIT_MAX_MESSAGE_NOTIFICATION,
                                      w.len);
            assert_valid_out(rsp, w.len);
        }
        free(m);
    }
    printf("handlers: %ld/%ld mutants answered\n", handled, iters());
    xboard_free(b);
}

/* ---------------------------------------------------------------- M2: transfer sequences */

static void test_fuzz_transfer_sequences(void)
{
    xboard_t *b = xboard_from_vectors("{\"storage_total\": 1200, \"journal_every\": 50}");
    /* two versions of asset "a", one of "b", one firmware image */
    static uint8_t data[4][300];
    const size_t len[4] = {200, 120, 90, 300};
    for (int k = 0; k < 3; k++) {
        memcpy(data[k], "ESPA\x01\x03\x04\x00\x00\x00\x00\x00", 12);
        proto_wr32(data[k] + 12, (uint32_t)len[k] - 16);
        for (size_t j = 16; j < len[k]; j++) {
            data[k][j] = (uint8_t)(j * (k + 3));
        }
    }
    for (size_t j = 0; j < len[3]; j++) {
        data[3][j] = (uint8_t)j;
    }
    const char *ids[4] = {"a", "a", "b", "firmware"};
    uint8_t handle = 1;
    uint32_t next_off = 0;
    int cur = 0;
    long finished = 0;
    for (long i = 0; i < iters() / 4; i++) {
        uint8_t msg[256];
        proto_writer_t w;
        proto_writer_init(&w, msg, sizeof(msg));
        uint32_t op = below(100);
        if (op < 4) { /* BEGIN */
            cur = (int)below(4);
            uint8_t sha[32];
            proto_sha256(data[cur], len[cur], sha);
            proto_w_header(&w, PROTO_MSG_XFER_BEGIN, 1);
            proto_w_u8(&w, PROTO_TAG_XFER_BEGIN_KIND, cur == 3 ? 2 : 1);
            proto_w_str(&w, PROTO_TAG_XFER_BEGIN_ID, ids[cur]);
            if (cur != 3) {
                proto_w_u8(&w, PROTO_TAG_XFER_BEGIN_ASSET_TYPE, 3);
            }
            proto_w_u32(&w, PROTO_TAG_XFER_BEGIN_SIZE, (uint32_t)len[cur]);
            proto_w_u32(&w, PROTO_TAG_XFER_BEGIN_CRC32, proto_crc32_update(0, data[cur], len[cur]) ^ (below(20) == 0));
            proto_w_bytes(&w, PROTO_TAG_XFER_BEGIN_SHA256, sha, 32);
            proto_w_str(&w, PROTO_TAG_XFER_BEGIN_VERSION, "9.0.0");
            if (below(5) == 0) {
                proto_w_bool(&w, PROTO_TAG_XFER_BEGIN_RESTART, true);
            }
        } else if (op < 80) { /* CHUNK, mostly in order */
            uint32_t off = below(8) ? next_off : below((uint32_t)len[cur] + 10);
            uint32_t n = 1 + below(70);
            if (off > len[cur]) {
                off = (uint32_t)len[cur];
            }
            if (off + n > len[cur] + (below(10) == 0)) {
                n = (uint32_t)len[cur] - off + (below(10) == 0);
            }
            if (n == 0) {
                n = 1;
            }
            uint8_t piece[80] = {0};
            memcpy(piece, data[cur] + off, off + n <= len[cur] ? n : len[cur] - off);
            proto_w_header(&w, PROTO_MSG_XFER_CHUNK, 0);
            proto_w_u8(&w, PROTO_TAG_XFER_CHUNK_HANDLE, below(10) ? handle : (uint8_t)rnd());
            proto_w_u32(&w, PROTO_TAG_XFER_CHUNK_OFFSET, off);
            proto_w_bytes(&w, PROTO_TAG_XFER_CHUNK_DATA, piece, n);
        } else if (op < 88) {
            static const uint8_t types[] = {PROTO_MSG_XFER_FINISH, PROTO_MSG_XFER_STATUS, PROTO_MSG_XFER_ABORT};
            proto_w_header(&w, types[below(3)], 2);
            proto_w_u8(&w, 1, handle);
        } else if (op < 92) {
            proto_w_header(&w, below(2) ? PROTO_MSG_ASSET_LIST : PROTO_MSG_STORAGE_INFO, 3);
        } else if (op < 94) {
            proto_w_header(&w, PROTO_MSG_ASSET_DELETE, 4);
            proto_w_u8(&w, PROTO_TAG_ASSET_DELETE_ASSET_TYPE, 3);
            proto_w_str(&w, PROTO_TAG_ASSET_DELETE_ID, below(2) ? "a" : "b");
        } else if (op < 96) {
            xfer_on_disconnect(&b->x);
            continue;
        } else if (op < 98) {
            xboard_power_limit(b, (int32_t)below(6)); /* the power will fail soon */
            continue;
        } else {
            xboard_reboot(b);
            continue;
        }
        xboard_request(b, msg, w.len);
        for (int k = 0; k < b->n_out; k++) {
            proto_msg_t m;
            assert_valid_out(b->out[k], b->out_len[k]);
            proto_msg_parse(b->out[k], b->out_len[k], &m);
            uint8_t v8;
            uint32_t v32;
            if (m.type == PROTO_MSG_XFER_BEGIN_RSP && proto_get_u8(m.body, m.body_len, 2, &v8)) {
                TEST_ASSERT_NOT_EQUAL(0, v8);
                handle = v8;
                proto_get_u32(m.body, m.body_len, PROTO_TAG_XFER_BEGIN_RSP_OFFSET, &next_off);
            }
            if (m.type == PROTO_MSG_XFER_ACK && proto_get_u32(m.body, m.body_len, PROTO_TAG_XFER_ACK_OFFSET, &v32)) {
                next_off = v32 ? v32 : next_off;
            }
            if (m.type == PROTO_MSG_XFER_FINISH_RSP && proto_get_u8(m.body, m.body_len, 1, &v8) && v8 == PROTO_OK) {
                finished++;
            }
        }
        /* an installed asset is always exactly one complete version */
        uint32_t size;
        const uint8_t *d = blob_mem_data(&b->blob, "3_a", &size);
        if (d) {
            TEST_ASSERT_TRUE((size == len[0] && memcmp(d, data[0], size) == 0) ||
                             (size == len[1] && memcmp(d, data[1], size) == 0));
        }
        d = blob_mem_data(&b->blob, "3_b", &size);
        if (d) {
            TEST_ASSERT_TRUE(size == len[2] && memcmp(d, data[2], size) == 0);
        }
    }
    printf("transfer sequences: %ld successful FINISH\n", finished);
    TEST_ASSERT_GREATER_THAN(0, finished);
    xboard_free(b);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_message_parse);
    RUN_TEST(test_fuzz_text);
    RUN_TEST(test_fuzz_segments);
    RUN_TEST(test_fuzz_ancs);
    RUN_TEST(test_fuzz_storage_records);
    RUN_TEST(test_fuzz_handlers);
    RUN_TEST(test_fuzz_transfer_sequences);
    for (size_t i = 0; i < n_seeds; i++) {
        free(seeds[i]);
    }
    return UNITY_END();
}
