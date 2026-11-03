/* transfer + xfer_ota unit tests: power loss, OTA states, version/battery gates, asset formats,
 * storage edge cases. The protocol-visible behaviour is covered by transfer.json (test_vectors). */
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <string.h>

#include "epager/blob_fs.h"
#include "test_support.h"
#include "tmp_fs.h"
#include "unity.h"
#include "xboard.h"

#define THEME 3
#define THEME_V1 4

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------- helpers */

static size_t container(uint8_t *out, uint8_t type, uint8_t fmt, uint16_t w, uint16_t h, size_t payload, uint8_t seed)
{
    memcpy(out, "ESPA", 4);
    out[4] = 1;
    out[5] = type;
    out[6] = fmt;
    out[7] = 0;
    proto_wr16(out + 8, w);
    proto_wr16(out + 10, h);
    proto_wr32(out + 12, (uint32_t)payload);
    for (size_t i = 0; i < payload; i++) {
        out[16 + i] = (uint8_t)(i * 37 + seed);
    }
    return 16 + payload;
}

static proto_status_t out_status(const xboard_t *b, int k)
{
    proto_msg_t m;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, proto_msg_parse(b->out[k], b->out_len[k], &m));
    uint8_t st = 0xFF;
    proto_get_u8(m.body, m.body_len, m.type == PROTO_MSG_XFER_ACK ? PROTO_TAG_XFER_ACK_STATUS : 1, &st);
    return (proto_status_t)st;
}

static uint32_t out_u32(const xboard_t *b, int k, uint8_t tag)
{
    proto_msg_t m;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, proto_msg_parse(b->out[k], b->out_len[k], &m));
    uint32_t v = 0xFFFFFFFF;
    if (!proto_get_u32(m.body, m.body_len, tag, &v)) {
        uint16_t v16;
        uint8_t v8;
        if (proto_get_u16(m.body, m.body_len, tag, &v16)) {
            v = v16;
        } else if (proto_get_u8(m.body, m.body_len, tag, &v8)) {
            v = v8;
        }
    }
    return v;
}

static void send(xboard_t *b, uint8_t *msg, proto_writer_t *w)
{
    TEST_ASSERT_TRUE(proto_writer_ok(w));
    xboard_request(b, msg, w->len);
}

typedef struct {
    proto_status_t st;
    uint8_t handle;
    uint32_t offset, prefix_crc;
} begin_t;

static begin_t begin(xboard_t *b, uint8_t kind, uint8_t type, const char *id, const uint8_t *data, size_t len,
                     const char *version, bool restart)
{
    uint8_t msg[256], sha[PROTO_SHA256_LEN];
    proto_writer_t w;
    proto_sha256(data, len, sha);
    proto_writer_init(&w, msg, sizeof(msg));
    proto_w_header(&w, PROTO_MSG_XFER_BEGIN, 1);
    proto_w_u8(&w, PROTO_TAG_XFER_BEGIN_KIND, kind);
    proto_w_str(&w, PROTO_TAG_XFER_BEGIN_ID, id);
    if (type) {
        proto_w_u8(&w, PROTO_TAG_XFER_BEGIN_ASSET_TYPE, type);
    }
    proto_w_u32(&w, PROTO_TAG_XFER_BEGIN_SIZE, (uint32_t)len);
    proto_w_u32(&w, PROTO_TAG_XFER_BEGIN_CRC32, proto_crc32_update(0, data, len));
    proto_w_bytes(&w, PROTO_TAG_XFER_BEGIN_SHA256, sha, sizeof(sha));
    if (version) {
        proto_w_str(&w, PROTO_TAG_XFER_BEGIN_VERSION, version);
    }
    if (restart) {
        proto_w_bool(&w, PROTO_TAG_XFER_BEGIN_RESTART, true);
    }
    send(b, msg, &w);
    TEST_ASSERT_EQUAL_INT(1, b->n_out);
    begin_t r = {out_status(b, 0), 0, 0, 0};
    if (r.st == PROTO_OK) {
        r.handle = (uint8_t)out_u32(b, 0, PROTO_TAG_XFER_BEGIN_RSP_HANDLE);
        r.offset = out_u32(b, 0, PROTO_TAG_XFER_BEGIN_RSP_OFFSET);
        r.prefix_crc = out_u32(b, 0, PROTO_TAG_XFER_BEGIN_RSP_PREFIX_CRC32);
    }
    return r;
}

static begin_t begin_asset(xboard_t *b, const char *id, const uint8_t *data, size_t len)
{
    return begin(b, PROTO_TRANSFER_KIND_ASSET, THEME, id, data, len, "1", false);
}

/* Returns the status of the last ack (OK if none). */
static proto_status_t chunk(xboard_t *b, uint8_t handle, uint32_t offset, const uint8_t *data, size_t len)
{
    uint8_t msg[PROTO_LIMIT_MAX_MESSAGE_TRANSFER + 64];
    proto_writer_t w;
    proto_writer_init(&w, msg, sizeof(msg));
    proto_w_header(&w, PROTO_MSG_XFER_CHUNK, 0);
    proto_w_u8(&w, PROTO_TAG_XFER_CHUNK_HANDLE, handle);
    proto_w_u32(&w, PROTO_TAG_XFER_CHUNK_OFFSET, offset);
    proto_w_bytes(&w, PROTO_TAG_XFER_CHUNK_DATA, data, len);
    send(b, msg, &w);
    return b->n_out ? out_status(b, b->n_out - 1) : PROTO_OK;
}

static proto_status_t send_range(xboard_t *b, uint8_t handle, const uint8_t *data, uint32_t from, uint32_t to)
{
    proto_status_t st = PROTO_OK;
    for (uint32_t off = from; off < to && st == PROTO_OK; off += 64) {
        st = chunk(b, handle, off, data + off, to - off < 64 ? to - off : 64);
    }
    return st;
}

static proto_status_t simple(xboard_t *b, uint8_t type, uint8_t handle)
{
    uint8_t msg[16];
    proto_writer_t w;
    proto_writer_init(&w, msg, sizeof(msg));
    proto_w_header(&w, type, 9);
    proto_w_u8(&w, 1, handle);
    send(b, msg, &w);
    TEST_ASSERT_EQUAL_INT(1, b->n_out);
    return out_status(b, 0);
}

static proto_status_t finish(xboard_t *b, uint8_t h) { return simple(b, PROTO_MSG_XFER_FINISH, h); }

static void install(xboard_t *b, const char *id, const uint8_t *data, size_t len)
{
    begin_t r = begin_asset(b, id, data, len);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, r.st);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, send_range(b, r.handle, data, r.offset, (uint32_t)len));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, finish(b, r.handle));
}

static int count_suffix(const xboard_t *b, char suffix)
{
    int n = 0;
    for (size_t i = 0; i < BLOB_MEM_MAX_FILES; i++) {
        size_t l = b->blob.f[i].used ? strlen(b->blob.f[i].name) : 0;
        n += l && b->blob.f[i].name[l - 1] == suffix;
    }
    return n;
}

/* ---------------------------------------------------------------- power loss */

static void test_power_loss_never_half_installs_an_asset(void)
{
    uint8_t old[200], neu[300];
    size_t old_len = container(old, THEME, THEME_V1, 0, 0, 184, 1);
    size_t new_len = container(neu, THEME, THEME_V1, 0, 0, 284, 2);
    int32_t budget = 0;
    for (;; budget++) {
        xboard_t *b = xboard_from_vectors("{\"journal_every\": 64}");
        install(b, "night", old, old_len);
        xboard_power_limit(b, budget);
        begin_t r = begin_asset(b, "night", neu, new_len);
        if (r.st == PROTO_OK) {
            send_range(b, r.handle, neu, r.offset, (uint32_t)new_len);
            finish(b, r.handle);
        }
        bool survived = b->power > 0;
        xboard_reboot(b); /* power cut, then boot */

        uint32_t size;
        const uint8_t *data = blob_mem_data(&b->blob, "3_night", &size);
        TEST_ASSERT_NOT_NULL(data);
        bool is_old = size == old_len && memcmp(data, old, old_len) == 0;
        bool is_new = size == new_len && memcmp(data, neu, new_len) == 0;
        TEST_ASSERT_TRUE_MESSAGE(is_old || is_new, "installed asset is a mix");
        TEST_ASSERT_LESS_OR_EQUAL(1, count_suffix(b, '~')); /* only the journaled temp file survives boot */

        /* the app resumes or restarts and finishes */
        r = begin_asset(b, "night", neu, new_len);
        TEST_ASSERT_EQUAL_INT(PROTO_OK, r.st);
        TEST_ASSERT_EQUAL_HEX32(proto_crc32_update(0, neu, r.offset), r.prefix_crc);
        TEST_ASSERT_EQUAL_INT(PROTO_OK, send_range(b, r.handle, neu, r.offset, (uint32_t)new_len));
        TEST_ASSERT_EQUAL_INT(PROTO_OK, finish(b, r.handle));
        data = blob_mem_data(&b->blob, "3_night", &size);
        TEST_ASSERT_TRUE(size == new_len && memcmp(data, neu, new_len) == 0);
        TEST_ASSERT_EQUAL_INT(0, count_suffix(b, '~'));
        xboard_free(b);
        if (survived) {
            break;
        }
    }
    printf("asset power-loss sweep: %d cut points\n", (int)budget + 1);
    TEST_ASSERT_GREATER_THAN(8, budget); /* the sweep crossed BEGIN, chunks, journal writes and the install */
}

static void test_power_loss_during_firmware_transfer_resumes(void)
{
    static uint8_t img[1000];
    for (size_t i = 0; i < sizeof(img); i++) {
        img[i] = (uint8_t)(i * 13 + 7);
    }
    int32_t budget = 0;
    for (;; budget++) {
        xboard_t *b = xboard_from_vectors("{\"firmware\": {\"slot_size\": 1000}}");
        xboard_power_limit(b, budget);
        begin_t r = begin(b, PROTO_TRANSFER_KIND_FIRMWARE, 0, "firmware", img, sizeof(img), "0.2.0", false);
        if (r.st == PROTO_OK) {
            send_range(b, r.handle, img, r.offset, sizeof(img));
            finish(b, r.handle);
        }
        bool survived = b->power > 0;
        xboard_reboot(b);
        if (!survived) {
            r = begin(b, PROTO_TRANSFER_KIND_FIRMWARE, 0, "firmware", img, sizeof(img), "0.2.0", false);
            TEST_ASSERT_EQUAL_INT(PROTO_OK, r.st);
            TEST_ASSERT_EQUAL_HEX32(proto_crc32_update(0, img, r.offset), r.prefix_crc);
            TEST_ASSERT_EQUAL_INT(PROTO_OK, send_range(b, r.handle, img, r.offset, sizeof(img)));
            TEST_ASSERT_EQUAL_INT(PROTO_OK, finish(b, r.handle));
        }
        TEST_ASSERT_TRUE(b->fw.boot_set);
        TEST_ASSERT_EQUAL_MEMORY(img, b->fw_buf, sizeof(img));
        xboard_free(b);
        if (survived) {
            break;
        }
    }
    printf("firmware power-loss sweep: %d cut points\n", (int)budget + 1);
    TEST_ASSERT_GREATER_THAN(16, budget);
}

static void test_install_failure_is_retryable(void)
{
    xboard_t *b = xboard_from_vectors(NULL);
    uint8_t a[100];
    size_t n = container(a, THEME, THEME_V1, 0, 0, 84, 3);
    begin_t r = begin_asset(b, "x", a, n);
    send_range(b, r.handle, a, 0, (uint32_t)n);
    xboard_power_limit(b, 0); /* the rename fails */
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, finish(b, r.handle));
    b->kv.power = b->blob.power = b->fw.power = NULL;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, finish(b, r.handle));
    uint32_t size;
    TEST_ASSERT_NOT_NULL(blob_mem_data(&b->blob, "3_x", &size));
    TEST_ASSERT_NOT_NULL(blob_mem_data(&b->blob, "3_x@", &size));
    xboard_free(b);
}

/* Found by test_fuzz_transfer_sequences: a shorter new file must never inherit a stale tail. */
static void test_stale_temp_data_is_never_installed(void)
{
    xboard_t *b = xboard_from_vectors(NULL);
    uint8_t v0[200], v1[120];
    size_t n0 = container(v0, THEME, THEME_V1, 0, 0, 184, 1), n1 = container(v1, THEME, THEME_V1, 0, 0, 104, 2);
    begin_t r = begin_asset(b, "a", v0, n0);
    send_range(b, r.handle, v0, 0, (uint32_t)n0); /* complete, not finished */
    xboard_power_limit(b, 0);
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, begin_asset(b, "a", v1, n1).st); /* stale data cannot be removed */
    b->kv.power = b->blob.power = b->fw.power = NULL;
    r = begin_asset(b, "a", v1, n1);
    TEST_ASSERT_EQUAL_UINT32(0, r.offset);
    send_range(b, r.handle, v1, 0, (uint32_t)n1);
    b->blob.iface.write(b->blob.iface.ctx, "3_a~", (uint32_t)n1, "tail", 4); /* a tail written behind our back */
    TEST_ASSERT_EQUAL_INT(PROTO_CRC_MISMATCH, finish(b, r.handle));
    uint32_t size;
    TEST_ASSERT_NULL(blob_mem_data(&b->blob, "3_a", &size));
    xboard_free(b);
}

/* ---------------------------------------------------------------- session details */

static void test_handles_wrap_and_never_zero(void)
{
    xboard_t *b = xboard_from_vectors(NULL);
    uint8_t a[40];
    size_t n = container(a, THEME, THEME_V1, 0, 0, 24, 1);
    for (int i = 1; i <= 256; i++) {
        begin_t r = begin_asset(b, "x", a, n);
        TEST_ASSERT_EQUAL_UINT8(i <= 255 ? i : i - 255, r.handle);
    }
    xboard_free(b);
}

static void test_invalid_chunk_is_acked(void)
{
    xboard_t *b = xboard_from_vectors(NULL);
    uint8_t a[200];
    size_t n = container(a, THEME, THEME_V1, 0, 0, 184, 1);
    begin_t r = begin_asset(b, "x", a, n);
    chunk(b, r.handle, 0, a, 64);

    static uint8_t big[PROTO_LIMIT_CHUNK_DATA_MAX + 1];
    TEST_ASSERT_EQUAL_INT(PROTO_TOO_LARGE, chunk(b, r.handle, 64, big, sizeof(big)));
    TEST_ASSERT_EQUAL_UINT32(64, out_u32(b, 0, PROTO_TAG_XFER_ACK_OFFSET));
    TEST_ASSERT_EQUAL_UINT32(r.handle, out_u32(b, 0, PROTO_TAG_XFER_ACK_HANDLE));
    TEST_ASSERT_EQUAL_INT(PROTO_TOO_LARGE, chunk(b, 77, 64, big, sizeof(big))); /* other handle: offset 0 */
    TEST_ASSERT_EQUAL_UINT32(0, out_u32(b, 0, PROTO_TAG_XFER_ACK_OFFSET));

    uint8_t msg[16];
    proto_writer_t w;
    proto_writer_init(&w, msg, sizeof(msg));
    proto_w_header(&w, PROTO_MSG_XFER_CHUNK, 0);
    proto_w_u32(&w, PROTO_TAG_XFER_CHUNK_OFFSET, 64); /* no handle, no data */
    send(b, msg, &w);
    TEST_ASSERT_EQUAL_INT(1, b->n_out);
    TEST_ASSERT_EQUAL_INT(PROTO_MISSING_FIELD, out_status(b, 0));
    TEST_ASSERT_EQUAL_UINT32(0, out_u32(b, 0, PROTO_TAG_XFER_ACK_HANDLE));

    /* invalid requests other than chunks get a status-only response */
    proto_writer_init(&w, msg, sizeof(msg));
    proto_w_header(&w, PROTO_MSG_XFER_FINISH, 5);
    send(b, msg, &w);
    TEST_ASSERT_EQUAL_INT(PROTO_MISSING_FIELD, out_status(b, 0));
    xboard_free(b);
}

static void test_resume_needs_the_stored_data(void)
{
    xboard_t *b = xboard_from_vectors(NULL);
    uint8_t a[200];
    size_t n = container(a, THEME, THEME_V1, 0, 0, 184, 1);
    begin_t r = begin_asset(b, "x", a, n);
    send_range(b, r.handle, a, 0, 128);
    xfer_on_disconnect(&b->x);
    b->blob.iface.remove(b->blob.iface.ctx, "3_x~"); /* lost behind our back */
    TEST_ASSERT_EQUAL_UINT32(0, begin_asset(b, "x", a, n).offset);

    static uint8_t img[300];
    r = begin(b, PROTO_TRANSFER_KIND_FIRMWARE, 0, "firmware", img, sizeof(img), "0.2.0", false);
    send_range(b, r.handle, img, 0, 256);
    xboard_reboot(b);
    b->fw.written = 100; /* the sink cannot resume at 256 */
    r = begin(b, PROTO_TRANSFER_KIND_FIRMWARE, 0, "firmware", img, sizeof(img), "0.2.0", false);
    TEST_ASSERT_EQUAL_UINT32(0, r.offset);
    TEST_ASSERT_EQUAL_UINT32(0, r.prefix_crc);
    xboard_free(b);
}

static void test_corrupt_journal_is_ignored(void)
{
    xboard_t *b = xboard_from_vectors(NULL);
    const kv_storage_t *kv = &b->kv.iface;
    uint8_t a[100];
    size_t n = container(a, THEME, THEME_V1, 0, 0, 84, 1);
    const char *junk[] = {"J", "J\x01", "Jxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"};
    for (size_t i = 0; i < 3; i++) {
        kv->set(kv->ctx, XFER_JOURNAL_KEY, junk[i], strlen(junk[i]));
        xboard_reboot(b);
        TEST_ASSERT_EQUAL_UINT32(0, begin_asset(b, "x", a, n).offset);
    }
    /* a structurally valid journal whose offset is past its size */
    begin_t r = begin_asset(b, "x", a, n);
    send_range(b, r.handle, a, 0, 64);
    xfer_on_disconnect(&b->x);
    uint8_t rec[128];
    size_t len;
    kv->get(kv->ctx, XFER_JOURNAL_KEY, rec, sizeof(rec), &len);
    proto_wr32(rec + 44, 1000);
    kv->set(kv->ctx, XFER_JOURNAL_KEY, rec, len);
    TEST_ASSERT_EQUAL_UINT32(0, begin_asset(b, "x", a, n).offset);
    rec[2] = 9; /* unknown kind */
    kv->set(kv->ctx, XFER_JOURNAL_KEY, rec, len);
    TEST_ASSERT_EQUAL_UINT32(0, begin_asset(b, "x", a, n).offset);
    xboard_free(b);
}

static void test_boot_cleans_orphans(void)
{
    xboard_t *b = xboard_from_vectors(NULL);
    const blob_storage_t *s = &b->blob.iface;
    s->write(s->ctx, "3_orphan~", 0, "x", 1);
    s->write(s->ctx, "3_gone@", 0, "1.0", 3);
    s->write(s->ctx, "3_kept", 0, "data", 4);
    s->write(s->ctx, "3_kept@", 0, "2.0", 3);
    uint8_t a[200];
    size_t n = container(a, THEME, THEME_V1, 0, 0, 184, 1);
    begin_t r = begin_asset(b, "busy", a, n);
    send_range(b, r.handle, a, 0, 128);
    xboard_reboot(b);
    uint32_t size;
    TEST_ASSERT_NULL(blob_mem_data(&b->blob, "3_orphan~", &size));
    TEST_ASSERT_NULL(blob_mem_data(&b->blob, "3_gone@", &size));
    TEST_ASSERT_NOT_NULL(blob_mem_data(&b->blob, "3_kept", &size));
    TEST_ASSERT_NOT_NULL(blob_mem_data(&b->blob, "3_kept@", &size));
    TEST_ASSERT_NOT_NULL(blob_mem_data(&b->blob, "3_busy~", &size)); /* journaled: kept for resume */
    TEST_ASSERT_EQUAL_UINT32(128, begin_asset(b, "busy", a, n).offset);
    xboard_free(b);
}

static void test_storage_full_mid_transfer(void)
{
    xboard_t *b = xboard_from_vectors("{\"storage_total\": 300}");
    uint8_t a[250];
    size_t n = container(a, THEME, THEME_V1, 0, 0, 234, 1);
    begin_t r = begin_asset(b, "x", a, n);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, r.st);
    b->blob.iface.write(b->blob.iface.ctx, "9_other", 0, a, 100); /* someone else takes space */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, send_range(b, r.handle, a, 0, 192));
    TEST_ASSERT_EQUAL_INT(PROTO_NO_SPACE, chunk(b, r.handle, 192, a + 192, 58));
    TEST_ASSERT_EQUAL_UINT32(192, out_u32(b, 0, PROTO_TAG_XFER_ACK_OFFSET));
    xboard_free(b);
}

static void test_asset_formats(void)
{
    xboard_t *b = xboard_from_vectors("{\"storage_total\": 20000, \"asset_support\": ["
                                      "{\"asset_type\": 2, \"format\": 1, \"max_size\": 5016},"
                                      "{\"asset_type\": 1, \"format\": 3, \"max_size\": 1024}]}");
    static uint8_t img[5016];
    const struct {
        uint8_t type, fmt;
        uint16_t w, h;
        size_t payload;
        proto_status_t want;
    } cases[] = {
        {2, 1, 200, 200, 5000, PROTO_OK},          /* Paper background */
        {2, 1, 199, 200, 5000, PROTO_IMAGE_INVALID}, /* not the display size */
        {2, 1, 200, 200, 4999, PROTO_IMAGE_INVALID}, /* payload does not match the format */
        {1, 3, 0, 0, 500, PROTO_OK},               /* font: no dimensions */
        {1, 3, 8, 0, 500, PROTO_IMAGE_INVALID},
        {1, 1, 0, 0, 500, PROTO_IMAGE_INVALID},    /* format not supported for fonts */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t n = container(img, cases[i].type, cases[i].fmt, cases[i].w, cases[i].h, cases[i].payload, (uint8_t)i);
        begin_t r = begin(b, PROTO_TRANSFER_KIND_ASSET, cases[i].type, "a", img, n, NULL, false);
        TEST_ASSERT_EQUAL_INT(PROTO_OK, r.st);
        TEST_ASSERT_EQUAL_INT(PROTO_OK, send_range(b, r.handle, img, 0, (uint32_t)n));
        char msg[16];
        snprintf(msg, sizeof(msg), "case %zu", i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].want, finish(b, r.handle), msg);
    }
    xboard_free(b);
}

/* The board's real storage shape: the same upload through blob_fs on a directory (LittleFS on the board). */
static void test_app_icon_upload_on_a_file_system(void)
{
    char over[160];
    snprintf(over, sizeof(over), "{\"asset_support\": [{\"asset_type\": %d, \"format\": %d, \"max_size\": 88, "
             "\"width\": 24, \"height\": 24}]}", PROTO_ASSET_TYPE_APP_ICON, PROTO_ASSET_FORMAT_MONO1);
    xboard_t *b = xboard_from_vectors(over);
    tmp_fs_t fs;
    blob_fs_t bf;
    TEST_ASSERT_EQUAL_INT(0, tmp_fs_make(&fs, 4096));
    TEST_ASSERT_TRUE(blob_fs_init(&bf, fs.dir, tmp_fs_usage, &fs));
    b->params.blob = &bf.iface;
    xfer_init(&b->x, &b->params);

    const char *id = "0123456789abcdef0123456789abcdef";
    char name[BLOB_NAME_MAX + 1];
    xfer_asset_blob_name(PROTO_ASSET_TYPE_APP_ICON, id, strlen(id), name);
    uint8_t icon[88], got[88];
    size_t n = container(icon, PROTO_ASSET_TYPE_APP_ICON, PROTO_ASSET_FORMAT_MONO1, 24, 24, 72, 1);
    TEST_ASSERT_EQUAL_UINT32(88, n);

    /* an upload cut off half way resumes from the journal and installs */
    begin_t r = begin(b, PROTO_TRANSFER_KIND_ASSET, PROTO_ASSET_TYPE_APP_ICON, id, icon, n, "1", false);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, r.st);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, send_range(b, r.handle, icon, 0, 40));
    bf.iface.flush(bf.iface.ctx, "x"); /* (the link drops; the core flushes at disconnect) */
    xboard_reboot(b);
    r = begin(b, PROTO_TRANSFER_KIND_ASSET, PROTO_ASSET_TYPE_APP_ICON, id, icon, n, "1", false);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, r.st);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(40, r.offset);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, send_range(b, r.handle, icon, r.offset, (uint32_t)n));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, finish(b, r.handle));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, bf.iface.read(bf.iface.ctx, name, 0, got, n));
    TEST_ASSERT_EQUAL_MEMORY(icon, got, n);

    /* a second upload of the same id replaces it */
    n = container(icon, PROTO_ASSET_TYPE_APP_ICON, PROTO_ASSET_FORMAT_MONO1, 24, 24, 72, 9);
    r = begin(b, PROTO_TRANSFER_KIND_ASSET, PROTO_ASSET_TYPE_APP_ICON, id, icon, n, "2", false);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, r.st);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, send_range(b, r.handle, icon, r.offset, (uint32_t)n));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, finish(b, r.handle));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, bf.iface.read(bf.iface.ctx, name, 0, got, n));
    TEST_ASSERT_EQUAL_MEMORY(icon, got, n);
    char listed[BLOB_NAME_MAX + 1];
    uint32_t size, files = 0;
    while (bf.iface.list(bf.iface.ctx, files, listed, sizeof(listed), &size) == BLOB_OK) {
        TEST_ASSERT_NOT_EQUAL('~', listed[strlen(listed) - 1]); /* no temp file is left behind */
        files++;
    }
    TEST_ASSERT_GREATER_THAN_UINT32(0, files);
    xboard_free(b);
    tmp_fs_remove_dir(fs.dir);
}

static void test_asset_list_fits_channel(void)
{
    xboard_t *b = xboard_from_vectors("{\"storage_total\": 100000}");
    uint8_t a[16];
    size_t n = container(a, THEME, THEME_V1, 0, 0, 0, 0);
    char id[33], ver[33];
    memset(ver, 'v', 32);
    ver[32] = '\0';
    for (int i = 0; i < 20; i++) {
        memset(id, 'a' + i, 32);
        id[32] = '\0';
        begin_t r = begin(b, PROTO_TRANSFER_KIND_ASSET, THEME, id, a, n, ver, false);
        send_range(b, r.handle, a, 0, (uint32_t)n);
        TEST_ASSERT_EQUAL_INT(PROTO_OK, finish(b, r.handle));
    }
    uint8_t msg[16];
    proto_writer_t w;
    proto_writer_init(&w, msg, sizeof(msg));
    proto_w_header(&w, PROTO_MSG_ASSET_LIST, 3);
    send(b, msg, &w);
    TEST_ASSERT_LESS_OR_EQUAL(PROTO_LIMIT_MAX_MESSAGE_TRANSFER, b->out_len[0]);
    char *json = msg_to_json_str(b->out[0], b->out_len[0]);
    int entries = 0;
    for (const char *s = json; (s = strstr(s, "\"id\"")) != NULL; s++) {
        entries++;
    }
    TEST_ASSERT_EQUAL_INT(12, entries); /* 13 + 12 x 84 bytes fit in 1100, a 13th entry would not */
    TEST_ASSERT_NOT_NULL(strstr(json, "\"total\":20"));
    free(json);
    xboard_free(b);
}

static void test_board_without_assets_or_firmware(void)
{
    xboard_t *b = xboard_from_vectors("{\"firmware\": null}");
    b->params.blob = NULL;
    xfer_init(&b->x, &b->params);
    uint8_t a[40];
    size_t n = container(a, THEME, THEME_V1, 0, 0, 24, 1);
    TEST_ASSERT_EQUAL_INT(PROTO_UNSUPPORTED, begin_asset(b, "x", a, n).st);
    const uint8_t types[] = {PROTO_MSG_STORAGE_INFO, PROTO_MSG_ASSET_LIST};
    for (size_t i = 0; i < sizeof(types); i++) {
        uint8_t msg[8];
        proto_writer_t w;
        proto_writer_init(&w, msg, sizeof(msg));
        proto_w_header(&w, types[i], 1);
        send(b, msg, &w);
        TEST_ASSERT_EQUAL_INT(PROTO_UNSUPPORTED, out_status(b, 0));
    }
    uint8_t msg[32];
    proto_writer_t w;
    proto_writer_init(&w, msg, sizeof(msg));
    proto_w_header(&w, PROTO_MSG_ASSET_DELETE, 1);
    proto_w_u8(&w, PROTO_TAG_ASSET_DELETE_ASSET_TYPE, THEME);
    proto_w_str(&w, PROTO_TAG_ASSET_DELETE_ID, "x");
    send(b, msg, &w);
    TEST_ASSERT_EQUAL_INT(PROTO_UNSUPPORTED, out_status(b, 0));

    proto_writer_init(&w, msg, sizeof(msg));
    proto_w_header(&w, PROTO_MSG_PING, 1);
    proto_msg_t m;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, proto_msg_parse(msg, w.len, &m));
    TEST_ASSERT_FALSE(xfer_handle(&b->x, &m, &w));
    xboard_free(b);
}

static void test_max_chunk_clamped(void)
{
    xboard_t *b = xboard_from_vectors("{\"max_chunk\": 4000}");
    uint8_t a[40];
    size_t n = container(a, THEME, THEME_V1, 0, 0, 24, 1);
    begin_asset(b, "x", a, n);
    TEST_ASSERT_EQUAL_UINT32(PROTO_LIMIT_CHUNK_DATA_MAX, out_u32(b, 0, PROTO_TAG_XFER_BEGIN_RSP_MAX_CHUNK));
    xboard_free(b);
}

static void test_firmware_progress_and_reboot_events(void)
{
    xboard_t *b = xboard_from_vectors(NULL);
    static uint8_t img[300];
    begin_t r = begin(b, PROTO_TRANSFER_KIND_FIRMWARE, 0, "firmware", img, sizeof(img), "0.2.0", false);
    send_range(b, r.handle, img, 0, sizeof(img));
    TEST_ASSERT_EQUAL_INT(5, b->ota_events[XFER_OTA_EV_PROGRESS]); /* 21, 42, 64, 85, 100 % */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, finish(b, r.handle));
    TEST_ASSERT_EQUAL_INT(1, b->ota_events[XFER_OTA_EV_REBOOT]);
    uint8_t rec[40];
    size_t n;
    TEST_ASSERT_EQUAL_INT(KV_OK, b->kv.iface.get(b->kv.iface.ctx, XFER_OTA_KV_KEY, rec, sizeof(rec), &n));
    TEST_ASSERT_EQUAL_MEMORY("0.2.0", rec + 3, 5);
    xboard_free(b);
}

static void test_firmware_begin_busy_while_another_update_writes(void)
{
    xboard_t *b = xboard_from_vectors(NULL);
    static uint8_t img[300];
    b->fw.begin_status = PROTO_BUSY; /* the Wi-Fi update owns the sink */
    begin_t r = begin(b, PROTO_TRANSFER_KIND_FIRMWARE, 0, "firmware", img, sizeof(img), "0.2.0", false);
    TEST_ASSERT_EQUAL_INT(PROTO_BUSY, r.st);
    b->fw.begin_status = PROTO_INTERNAL; /* any other refusal is still INTERNAL */
    r = begin(b, PROTO_TRANSFER_KIND_FIRMWARE, 0, "firmware", img, sizeof(img), "0.2.0", false);
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, r.st);
    b->fw.begin_status = PROTO_OK;
    r = begin(b, PROTO_TRANSFER_KIND_FIRMWARE, 0, "firmware", img, sizeof(img), "0.2.0", false);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, r.st);
    xboard_free(b);
}

/* ---------------------------------------------------------------- OTA state machine */

static kv_mem_t okv;
static int ota_ev[5];
static uint8_t ota_last_state;
static bool batt_ok;
static uint8_t batt_pct, batt_ext;

static void ota_on_event(void *ctx, const xfer_ota_event_t *ev)
{
    (void)ctx;
    ota_ev[ev->kind]++;
    if (ev->kind == XFER_OTA_EV_STATE) {
        ota_last_state = ev->state;
    }
}

static bool ota_battery(void *ctx, uint8_t *pct, uint8_t *ext)
{
    (void)ctx;
    *pct = batt_pct;
    *ext = batt_ext;
    return batt_ok;
}

static xfer_ota_t ota_boot(const char *running, uint32_t required, bool pending, uint32_t now)
{
    xfer_ota_params_t p = {.running_version = running, .low_battery_pct = 30, .kv = &okv.iface,
                           .required_checks = required, .verify_timeout_ms = 1000, .on_event = ota_on_event};
    xfer_ota_t o;
    memset(ota_ev, 0, sizeof(ota_ev));
    xfer_ota_init(&o, &p);
    xfer_ota_boot(&o, pending, now);
    return o;
}

static void test_ota_update_verified(void)
{
    kv_mem_init(&okv);
    xfer_ota_t o = ota_boot("0.1.0", 7, false, 0);
    TEST_ASSERT_EQUAL_UINT8(PROTO_OTA_STATE_NONE, xfer_ota_state(&o));
    xfer_ota_installed(&o, "0.2.0", 5);
    TEST_ASSERT_EQUAL_INT(1, ota_ev[XFER_OTA_EV_REBOOT]);

    o = ota_boot("0.2.0", 7, true, 500); /* the new image, not yet valid */
    TEST_ASSERT_EQUAL_UINT8(PROTO_OTA_STATE_PENDING_VERIFY, xfer_ota_state(&o));
    xfer_ota_check_passed(&o, 1);
    xfer_ota_check_passed(&o, 2);
    xfer_ota_tick(&o, 1499);
    TEST_ASSERT_EQUAL_INT(0, ota_ev[XFER_OTA_EV_MARK_VALID] + ota_ev[XFER_OTA_EV_ROLLBACK]);
    xfer_ota_check_passed(&o, 4);
    TEST_ASSERT_EQUAL_INT(1, ota_ev[XFER_OTA_EV_MARK_VALID]);
    TEST_ASSERT_EQUAL_UINT8(PROTO_OTA_STATE_UPDATED, ota_last_state);
    xfer_ota_tick(&o, 100000);
    xfer_ota_check_failed(&o);
    TEST_ASSERT_EQUAL_INT(0, ota_ev[XFER_OTA_EV_ROLLBACK]);
    TEST_ASSERT_EQUAL_UINT(0, kv_mem_count(&okv)); /* record cleared */

    o = ota_boot("0.2.0", 7, false, 0); /* next boot: nothing to report */
    TEST_ASSERT_EQUAL_UINT8(PROTO_OTA_STATE_NONE, xfer_ota_state(&o));
}

static void test_ota_self_check_failure_rolls_back(void)
{
    kv_mem_init(&okv);
    xfer_ota_t o = ota_boot("0.1.0", 3, false, 0);
    xfer_ota_installed(&o, "0.2.0", 5);
    o = ota_boot("0.2.0", 3, true, 0);
    xfer_ota_check_passed(&o, 1);
    xfer_ota_check_failed(&o);
    xfer_ota_check_failed(&o);
    xfer_ota_check_passed(&o, 2); /* too late */
    TEST_ASSERT_EQUAL_INT(1, ota_ev[XFER_OTA_EV_ROLLBACK]);
    TEST_ASSERT_EQUAL_INT(0, ota_ev[XFER_OTA_EV_MARK_VALID]);
    o = ota_boot("0.1.0", 3, false, 0); /* the bootloader went back to the old image */
    TEST_ASSERT_EQUAL_UINT8(PROTO_OTA_STATE_ROLLED_BACK, xfer_ota_state(&o));
    o = ota_boot("0.1.0", 3, false, 0);
    TEST_ASSERT_EQUAL_UINT8(PROTO_OTA_STATE_NONE, xfer_ota_state(&o));
}

static void test_ota_verify_timeout(void)
{
    kv_mem_init(&okv);
    xfer_ota_t o = ota_boot("0.2.0", 1, true, 0xFFFFFF00u); /* timer wraps */
    xfer_ota_tick(&o, 0xFFFFFF00u + 999);
    TEST_ASSERT_EQUAL_INT(0, ota_ev[XFER_OTA_EV_ROLLBACK]);
    xfer_ota_tick(&o, 0xFFFFFF00u + 1000);
    xfer_ota_tick(&o, 0xFFFFFF00u + 5000);
    TEST_ASSERT_EQUAL_INT(1, ota_ev[XFER_OTA_EV_ROLLBACK]);
}

static void test_ota_marked_valid_before_power_loss(void)
{
    kv_mem_init(&okv);
    okv.iface.set(okv.iface.ctx, XFER_OTA_KV_KEY, "O\x01\x05" "0.2.0", 8);
    xfer_ota_t o = ota_boot("0.2.0", 1, false, 0);
    TEST_ASSERT_EQUAL_UINT8(PROTO_OTA_STATE_UPDATED, xfer_ota_state(&o));
    okv.iface.set(okv.iface.ctx, XFER_OTA_KV_KEY, "O\x01\x09" "0.2.0", 8); /* corrupt: ignored */
    o = ota_boot("0.2.0", 1, false, 0);
    TEST_ASSERT_EQUAL_UINT8(PROTO_OTA_STATE_NONE, xfer_ota_state(&o));
    o = ota_boot("0.3.0", 0, true, 0); /* nothing required: valid at once */
    TEST_ASSERT_EQUAL_INT(1, ota_ev[XFER_OTA_EV_MARK_VALID]);
    TEST_ASSERT_EQUAL_UINT8(PROTO_OTA_STATE_UPDATED, xfer_ota_state(&o));
}

static void test_ota_progress_events(void)
{
    xfer_ota_t o = ota_boot("0.1.0", 0, false, 0);
    for (uint32_t i = 0; i <= 1000; i++) {
        xfer_ota_progress(&o, i, 1000);
    }
    TEST_ASSERT_EQUAL_INT(101, ota_ev[XFER_OTA_EV_PROGRESS]);
    xfer_ota_progress(&o, 0, 0);
    TEST_ASSERT_EQUAL_INT(101, ota_ev[XFER_OTA_EV_PROGRESS]); /* 100 % again: no event */
}

static void test_ota_gates(void)
{
    xfer_ota_params_t p = {.running_version = "0.2.0", .low_battery_pct = 30, .kv = &okv.iface};
    xfer_ota_t o;
    xfer_ota_init(&o, &p);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, xfer_ota_check_battery(&o)); /* no reader */
    TEST_ASSERT_EQUAL_INT(PROTO_VERSION_REJECTED, xfer_ota_check_begin(&o, "0.3.0", 5, false));
    TEST_ASSERT_EQUAL_INT(PROTO_VERSION_REJECTED, xfer_ota_check_begin(&o, "0.2.0", 5, true));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, xfer_ota_check_begin(&o, "0.2.1-rc.1", 10, true));
    p.read_battery = ota_battery;
    xfer_ota_init(&o, &p);
    const struct {
        bool ok;
        uint8_t pct, ext;
        proto_status_t want;
    } cases[] = {
        {false, 100, PROTO_EXTERNAL_POWER_NO, PROTO_LOW_BATTERY}, {true, 29, PROTO_EXTERNAL_POWER_NO, PROTO_LOW_BATTERY},
        {true, 30, PROTO_EXTERNAL_POWER_NO, PROTO_OK},            {true, 0, PROTO_EXTERNAL_POWER_YES, PROTO_OK},
        {true, 29, PROTO_EXTERNAL_POWER_UNKNOWN, PROTO_LOW_BATTERY},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        batt_ok = cases[i].ok;
        batt_pct = cases[i].pct;
        batt_ext = cases[i].ext;
        TEST_ASSERT_EQUAL_INT(cases[i].want, xfer_ota_check_battery(&o));
        TEST_ASSERT_EQUAL_INT(cases[i].want, xfer_ota_check_begin(&o, "1.0.0", 5, true));
    }
    batt_ok = true;
    batt_pct = 100;
    p.running_version = "not-semver";
    xfer_ota_init(&o, &p);
    TEST_ASSERT_EQUAL_INT(PROTO_VERSION_REJECTED, xfer_ota_check_begin(&o, "9.9.9", 5, true));
    p.allow_downgrade = true;
    xfer_ota_init(&o, &p);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, xfer_ota_check_begin(&o, "0.0.1", 5, true));
    TEST_ASSERT_EQUAL_INT(PROTO_VERSION_REJECTED, xfer_ota_check_begin(&o, "0.0", 3, true));
}

static int cmp(const char *a, const char *b)
{
    int c = 99;
    TEST_ASSERT_TRUE_MESSAGE(xfer_version_compare(a, strlen(a), b, strlen(b), &c), a);
    return c;
}

static void test_semver_precedence(void)
{
    /* semver.org 11 examples plus our extensions; mirrors tools/tests/test_reference.py */
    const char *chain[] = {"0.1.0", "0.1.1", "0.9.9", "0.10.0", "1.0.0-01", "1.0.0-1", "1.0.0-alpha",
                           "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11",
                           "1.0.0-rc.1", "1.0.0", "1.0.1", "999999999.0.0"};
    size_t n = sizeof(chain) / sizeof(chain[0]);
    for (size_t i = 0; i + 1 < n; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, cmp(chain[i], chain[i + 1]), chain[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmp(chain[i + 1], chain[i]), chain[i]);
        TEST_ASSERT_EQUAL_INT(0, cmp(chain[i], chain[i]));
    }
    TEST_ASSERT_EQUAL_INT(0, cmp("1.0.0+build.1", "1.0.0+other"));
    TEST_ASSERT_EQUAL_INT(-1, cmp("1.0.0-a.b", "1.0.0-a.b.c"));
    const char *bad[] = {"", "1", "1.2", "1.2.3.4", "v1.2.3", "1.2.3-", "1.2.3+", "1.2.3-a..b", "1.2.3-a b",
                         "1234567890.0.0", "1.2.3-1234567890", "1.2.3-\xc3\xa9", "1.2.3+a+b", "1.2.3-.a", "1..3"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int c;
        TEST_ASSERT_FALSE_MESSAGE(xfer_version_compare(bad[i], strlen(bad[i]), "1.0.0", 5, &c), bad[i]);
        TEST_ASSERT_FALSE_MESSAGE(xfer_version_compare("1.0.0", 5, bad[i], strlen(bad[i]), &c), bad[i]);
    }
}

/* ---------------------------------------------------------------- fakes */

static void test_blob_mem_edges(void)
{
    blob_mem_t m;
    blob_mem_init(&m, 10);
    const blob_storage_t *s = &m.iface;
    uint8_t buf[16];
    uint32_t size;
    char name[8];
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->read(s->ctx, "a", 0, buf, 1));
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->flush(s->ctx, "a"));
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->rename(s->ctx, "a", "b"));
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->write(s->ctx, "a", 1, "x", 1)); /* hole */
    TEST_ASSERT_EQUAL_INT(BLOB_NO_SPACE, s->write(s->ctx, "a", 0, "01234567890", 11));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "a", 0, "0123", 4));
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->read(s->ctx, "a", 2, buf, 3));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "b", 0, "xy", 2));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->rename(s->ctx, "b", "a")); /* replaces */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->stat(s->ctx, "a", &size));
    TEST_ASSERT_EQUAL_UINT32(2, size);
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->list(s->ctx, 0, name, 1, &size)); /* name buffer too small */
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->list(s->ctx, 1, name, sizeof(name), &size));
    char long_name[BLOB_NAME_MAX + 2];
    memset(long_name, 'n', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->write(s->ctx, long_name, 0, "", 0));
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->rename(s->ctx, "a", long_name));
    int32_t power = 0;
    m.power = &power;
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->write(s->ctx, "a", 0, "z", 1));
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->rename(s->ctx, "a", "c"));
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->remove(s->ctx, "a"));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->remove(s->ctx, "missing"));
    m.power = NULL;
    blob_mem_free(&m);

    blob_mem_init(&m, 1000);
    for (unsigned i = 0; i < BLOB_MEM_MAX_FILES; i++) {
        snprintf(name, sizeof(name), "f%u", i);
        TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, name, 0, "", 0));
    }
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->write(s->ctx, "full", 0, "", 0));
    TEST_ASSERT_NOT_NULL(blob_mem_data(&m, "f0", &size)); /* empty file */
    blob_mem_free(&m);
}

static void test_fw_mem_edges(void)
{
    uint8_t buf[8];
    fw_mem_t m;
    fw_mem_init(&m, buf, sizeof(buf));
    const xfer_fw_sink_t *s = &m.iface;
    TEST_ASSERT_EQUAL_INT(PROTO_NO_SPACE, s->write(s->ctx, 4, "12345", 5));
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, s->read(s->ctx, 9, buf, 1));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s->write(s->ctx, 0, "1234", 4));
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, s->resume(s->ctx, 8, 5));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s->resume(s->ctx, 8, 4));
    m.image_status = PROTO_IMAGE_INVALID;
    TEST_ASSERT_EQUAL_INT(PROTO_IMAGE_INVALID, s->finish(s->ctx, 8));
    TEST_ASSERT_FALSE(m.boot_set);
    int32_t power = 0;
    m.power = &power;
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, s->write(s->ctx, 0, "1", 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_power_loss_never_half_installs_an_asset);
    RUN_TEST(test_power_loss_during_firmware_transfer_resumes);
    RUN_TEST(test_install_failure_is_retryable);
    RUN_TEST(test_stale_temp_data_is_never_installed);
    RUN_TEST(test_handles_wrap_and_never_zero);
    RUN_TEST(test_invalid_chunk_is_acked);
    RUN_TEST(test_resume_needs_the_stored_data);
    RUN_TEST(test_corrupt_journal_is_ignored);
    RUN_TEST(test_boot_cleans_orphans);
    RUN_TEST(test_storage_full_mid_transfer);
    RUN_TEST(test_asset_formats);
    RUN_TEST(test_asset_list_fits_channel);
    RUN_TEST(test_board_without_assets_or_firmware);
    RUN_TEST(test_max_chunk_clamped);
    RUN_TEST(test_firmware_progress_and_reboot_events);
    RUN_TEST(test_firmware_begin_busy_while_another_update_writes);
    RUN_TEST(test_ota_update_verified);
    RUN_TEST(test_ota_self_check_failure_rolls_back);
    RUN_TEST(test_ota_verify_timeout);
    RUN_TEST(test_ota_marked_valid_before_power_loss);
    RUN_TEST(test_ota_progress_events);
    RUN_TEST(test_ota_gates);
    RUN_TEST(test_semver_precedence);
    RUN_TEST(test_blob_mem_edges);
    RUN_TEST(test_app_icon_upload_on_a_file_system);
    RUN_TEST(test_fw_mem_edges);
    return UNITY_END();
}
