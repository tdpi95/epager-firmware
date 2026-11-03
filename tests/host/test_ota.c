/* ota_sink over a fake flash that behaves like the OTA slot: erased bytes are 0xFF, a write can only clear
 * bits, and a sector is erased when a sequential write reaches it (esp_ota_begin with SEQUENTIAL_WRITES). */
#include <stdlib.h>
#include <string.h>

#include "epager/ota.h"
#include "unity.h"

#define SLOT 16384u
#define SECTOR 4096u

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    uint8_t mem[SLOT];
    bool open;
    uint32_t pos;
    ota_flash_status_t begin_st, resume_st, write_st, read_st;
    ota_end_t end_st;
    int aborts, ends;
    ota_flash_t iface;
} fake_t;

static ota_flash_status_t f_begin(void *ctx)
{
    fake_t *f = ctx;
    if (f->begin_st != OTA_FLASH_OK) {
        return f->begin_st;
    }
    f->open = true;
    f->pos = 0;
    return OTA_FLASH_OK;
}

static ota_flash_status_t f_resume(void *ctx, uint32_t offset)
{
    fake_t *f = ctx;
    if (f->resume_st != OTA_FLASH_OK) {
        return f->resume_st;
    }
    f->open = true;
    f->pos = offset;
    return OTA_FLASH_OK;
}

static ota_flash_status_t f_write(void *ctx, const void *data, size_t len)
{
    fake_t *f = ctx;
    if (f->write_st != OTA_FLASH_OK) {
        return f->write_st;
    }
    TEST_ASSERT_TRUE(f->open);
    TEST_ASSERT_TRUE(f->pos + len <= SLOT);
    uint32_t first = f->pos / SECTOR, last = (f->pos + (uint32_t)len - 1) / SECTOR;
    if (f->pos % SECTOR == 0) {
        memset(f->mem + first * SECTOR, 0xFF, (last - first + 1) * SECTOR);
    } else if (first != last) {
        memset(f->mem + (first + 1) * SECTOR, 0xFF, (last - first) * SECTOR);
    }
    for (size_t i = 0; i < len; i++) {
        f->mem[f->pos + i] &= ((const uint8_t *)data)[i];
    }
    f->pos += (uint32_t)len;
    return OTA_FLASH_OK;
}

static ota_flash_status_t f_read(void *ctx, uint32_t offset, void *buf, size_t len)
{
    fake_t *f = ctx;
    if (f->read_st != OTA_FLASH_OK) {
        return f->read_st;
    }
    memcpy(buf, f->mem + offset, len);
    return OTA_FLASH_OK;
}

static ota_end_t f_end(void *ctx, uint32_t size)
{
    fake_t *f = ctx;
    (void)size;
    f->ends++;
    f->open = false;
    return f->end_st;
}

static void f_abort(void *ctx)
{
    fake_t *f = ctx;
    f->aborts++;
    f->open = false;
}

static void fake_init(fake_t *f)
{
    memset(f, 0, sizeof(*f));
    memset(f->mem, 0xFF, sizeof(f->mem));
    f->iface = (ota_flash_t){.slot_size = SLOT, .begin = f_begin, .resume = f_resume, .write = f_write,
                             .read = f_read, .end = f_end, .abort = f_abort, .ctx = f};
}

static uint8_t image[SLOT];

static void make_image(size_t n)
{
    for (size_t i = 0; i < n; i++) {
        image[i] = (uint8_t)(i * 31 + 7);
    }
}

static proto_status_t put(const xfer_fw_sink_t *k, uint32_t from, uint32_t to, uint32_t chunk)
{
    for (uint32_t pos = from; pos < to;) {
        uint32_t n = to - pos < chunk ? to - pos : chunk;
        proto_status_t st = k->write(k->ctx, pos, image + pos, n);
        if (st != PROTO_OK) {
            return st;
        }
        pos += n;
    }
    return PROTO_OK;
}

static void test_slot_size_comes_from_the_flash(void)
{
    fake_t f;
    ota_sink_t s;
    fake_init(&f);
    ota_sink_init(&s, &f.iface);
    TEST_ASSERT_EQUAL_UINT32(SLOT, s.iface.slot_size);
}

static void test_whole_image_installs(void)
{
    fake_t f;
    ota_sink_t s;
    fake_init(&f);
    ota_sink_init(&s, &f.iface);
    make_image(10000);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.begin(s.iface.ctx, 10000));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, put(&s.iface, 0, 10000, 244));
    uint8_t back[10000];
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.read(s.iface.ctx, 0, back, sizeof(back)));
    TEST_ASSERT_EQUAL_MEMORY(image, back, 10000);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.finish(s.iface.ctx, 10000));
    TEST_ASSERT_EQUAL_INT(1, f.ends);
    TEST_ASSERT_FALSE(s.open);
}

static void test_write_rules(void)
{
    fake_t f;
    ota_sink_t s;
    fake_init(&f);
    ota_sink_init(&s, &f.iface);
    make_image(1000);
    TEST_ASSERT_EQUAL_INT(PROTO_NO_SESSION, s.iface.write(s.iface.ctx, 0, image, 10)); /* nothing begun */
    TEST_ASSERT_EQUAL_INT(PROTO_NO_SPACE, s.iface.begin(s.iface.ctx, SLOT + 1));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.begin(s.iface.ctx, 1000));
    TEST_ASSERT_EQUAL_INT(PROTO_BAD_OFFSET, s.iface.write(s.iface.ctx, 10, image, 10)); /* a hole */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.write(s.iface.ctx, 0, image, 100));
    TEST_ASSERT_EQUAL_INT(PROTO_BAD_OFFSET, s.iface.write(s.iface.ctx, 50, image, 10)); /* overlap */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.write(s.iface.ctx, 100, image + 100, 100));
    TEST_ASSERT_EQUAL_UINT32(200, s.written);
}

static void test_write_past_the_slot_is_refused(void)
{
    fake_t f;
    ota_sink_t s;
    fake_init(&f);
    ota_sink_init(&s, &f.iface);
    make_image(SLOT);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.begin(s.iface.ctx, SLOT));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, put(&s.iface, 0, SLOT - 10, 1000));
    TEST_ASSERT_EQUAL_INT(PROTO_NO_SPACE, s.iface.write(s.iface.ctx, SLOT - 10, image, 11));
    TEST_ASSERT_EQUAL_UINT32(SLOT - 10, s.written);
}

static void test_flash_statuses_map_to_protocol_statuses(void)
{
    fake_t f;
    ota_sink_t s;
    fake_init(&f);
    ota_sink_init(&s, &f.iface);
    make_image(100);
    f.begin_st = OTA_FLASH_BUSY;
    TEST_ASSERT_EQUAL_INT(PROTO_BUSY, s.iface.begin(s.iface.ctx, 100)); /* the running image is still on probation */
    TEST_ASSERT_FALSE(s.open);
    f.begin_st = OTA_FLASH_ERROR;
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, s.iface.begin(s.iface.ctx, 100));
    f.begin_st = OTA_FLASH_OK;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.begin(s.iface.ctx, 100));
    f.write_st = OTA_FLASH_BAD_IMAGE; /* the first byte is not an image header */
    TEST_ASSERT_EQUAL_INT(PROTO_IMAGE_INVALID, s.iface.write(s.iface.ctx, 0, image, 10));
    f.write_st = OTA_FLASH_ERROR;
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, s.iface.write(s.iface.ctx, 0, image, 10));
    TEST_ASSERT_EQUAL_UINT32(0, s.written); /* a failed write does not move the position */
}

static void test_finish_verdicts(void)
{
    static const struct {
        ota_end_t end;
        proto_status_t want;
    } cases[] = {{OTA_END_OK, PROTO_OK},
                 {OTA_END_BAD_IMAGE, PROTO_IMAGE_INVALID},
                 {OTA_END_BAD_SIGNATURE, PROTO_SIGNATURE_INVALID},
                 {OTA_END_ERROR, PROTO_INTERNAL}};
    make_image(500);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fake_t f;
        ota_sink_t s;
        fake_init(&f);
        ota_sink_init(&s, &f.iface);
        f.end_st = cases[i].end;
        TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.begin(s.iface.ctx, 500));
        TEST_ASSERT_EQUAL_INT(PROTO_OK, put(&s.iface, 0, 500, 200));
        TEST_ASSERT_EQUAL_INT(cases[i].want, s.iface.finish(s.iface.ctx, 500));
        TEST_ASSERT_FALSE(s.open); /* the handle is gone after any verdict */
    }
}

static void test_finish_needs_every_byte(void)
{
    fake_t f;
    ota_sink_t s;
    fake_init(&f);
    ota_sink_init(&s, &f.iface);
    make_image(500);
    TEST_ASSERT_EQUAL_INT(PROTO_NO_SESSION, s.iface.finish(s.iface.ctx, 500));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.begin(s.iface.ctx, 500));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, put(&s.iface, 0, 400, 200));
    TEST_ASSERT_EQUAL_INT(PROTO_IMAGE_INVALID, s.iface.finish(s.iface.ctx, 500));
    TEST_ASSERT_EQUAL_INT(0, f.ends); /* never asked the flash to verify a short image */
}

static void test_read_stays_inside_what_was_written(void)
{
    fake_t f;
    ota_sink_t s;
    uint8_t b[64];
    fake_init(&f);
    ota_sink_init(&s, &f.iface);
    make_image(500);
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, s.iface.read(s.iface.ctx, 0, b, 1)); /* nothing begun */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.begin(s.iface.ctx, 500));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, put(&s.iface, 0, 100, 100));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.read(s.iface.ctx, 36, b, 64));
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, s.iface.read(s.iface.ctx, 37, b, 64));
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, s.iface.read(s.iface.ctx, 101, b, 0));
    f.read_st = OTA_FLASH_ERROR;
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, s.iface.read(s.iface.ctx, 0, b, 1));
}

/* Power loss: a new sink (RAM is gone), the same flash. The journal is behind the flash by up to journal_every
 * bytes, so the phone resends bytes that are already there; with flash semantics they must land unchanged. */
static void test_resume_after_a_reboot_rewrites_the_tail(void)
{
    static const uint32_t journaled[] = {0, 1000, 4096, 4097, 8192, 9000};
    make_image(12000);
    for (size_t i = 0; i < sizeof(journaled) / sizeof(journaled[0]); i++) {
        fake_t f;
        ota_sink_t s, s2;
        fake_init(&f);
        ota_sink_init(&s, &f.iface);
        TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.begin(s.iface.ctx, 12000));
        TEST_ASSERT_EQUAL_INT(PROTO_OK, put(&s.iface, 0, 9500, 244)); /* then the power goes */
        ota_sink_init(&s2, &f.iface);
        TEST_ASSERT_EQUAL_INT(PROTO_OK, s2.iface.resume(s2.iface.ctx, 12000, journaled[i]));
        TEST_ASSERT_EQUAL_UINT32(journaled[i], s2.written);
        TEST_ASSERT_EQUAL_INT(PROTO_OK, put(&s2.iface, journaled[i], 12000, 244));
        TEST_ASSERT_EQUAL_MEMORY(image, f.mem, 12000);
        TEST_ASSERT_EQUAL_INT(PROTO_OK, s2.iface.finish(s2.iface.ctx, 12000));
    }
}

static void test_resume_checks(void)
{
    fake_t f;
    ota_sink_t s;
    fake_init(&f);
    ota_sink_init(&s, &f.iface);
    TEST_ASSERT_EQUAL_INT(PROTO_BAD_OFFSET, s.iface.resume(s.iface.ctx, 100, 101));
    TEST_ASSERT_EQUAL_INT(PROTO_BAD_OFFSET, s.iface.resume(s.iface.ctx, SLOT + 1, 0));
    f.resume_st = OTA_FLASH_ERROR;
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, s.iface.resume(s.iface.ctx, 100, 50));
    TEST_ASSERT_FALSE(s.open); /* a failed resume leaves nothing open: the transfer starts over with begin() */
    f.resume_st = OTA_FLASH_OK;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.resume(s.iface.ctx, 100, 100)); /* everything is there, only FINISH is left */
    TEST_ASSERT_EQUAL_UINT32(100, s.written);
}

static void test_abort_forgets_the_image(void)
{
    fake_t f;
    ota_sink_t s;
    fake_init(&f);
    ota_sink_init(&s, &f.iface);
    make_image(100);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, s.iface.begin(s.iface.ctx, 100));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, put(&s.iface, 0, 50, 50));
    s.iface.abort(s.iface.ctx);
    TEST_ASSERT_EQUAL_INT(1, f.aborts);
    TEST_ASSERT_EQUAL_UINT32(0, s.written);
    TEST_ASSERT_EQUAL_INT(PROTO_NO_SESSION, s.iface.write(s.iface.ctx, 0, image, 10));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_slot_size_comes_from_the_flash);
    RUN_TEST(test_whole_image_installs);
    RUN_TEST(test_write_rules);
    RUN_TEST(test_write_past_the_slot_is_refused);
    RUN_TEST(test_flash_statuses_map_to_protocol_statuses);
    RUN_TEST(test_finish_verdicts);
    RUN_TEST(test_finish_needs_every_byte);
    RUN_TEST(test_read_stays_inside_what_was_written);
    RUN_TEST(test_resume_after_a_reboot_rewrites_the_tail);
    RUN_TEST(test_resume_checks);
    RUN_TEST(test_abort_forgets_the_image);
    return UNITY_END();
}
