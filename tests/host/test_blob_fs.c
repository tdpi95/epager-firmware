/* blob_fs: the file-system blob_storage that the board runs over LittleFS, on a temp directory. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "epager/blob_fs.h"
#include "tmp_fs.h"
#include "unity.h"

/* esp_littlefs answers fstat() with lfs_stat(path): a file created or grown since its last sync has no
 * directory entry yet (ENOENT) or an old size. blob_fs keeps files open between chunks, so it must not
 * depend on fstat(); this build of the test makes every fstat() fail the way LittleFS does for such a file. */
int __real_fstat(int fd, struct stat *st);
int __wrap_fstat(int fd, struct stat *st);
int __wrap_fstat(int fd, struct stat *st)
{
    (void)fd;
    (void)st;
    errno = ENOENT;
    return -1;
}

/* A counter for stat(): a walk of the listing must not stat every file once per index (LittleFS answers each stat
 * by reading the directory's metadata log, which made the boot take 10 s with 40 files). */
int __real_stat(const char *path, struct stat *st);
int __wrap_stat(const char *path, struct stat *st);
static unsigned stat_calls;
int __wrap_stat(const char *path, struct stat *st)
{
    stat_calls++;
    return __real_stat(path, st);
}

static tmp_fs_t fs;
static blob_fs_t bf;
static const blob_storage_t *s;

void setUp(void)
{
    TEST_ASSERT_EQUAL_INT(0, tmp_fs_make(&fs, 1000));
    TEST_ASSERT_TRUE(blob_fs_init(&bf, fs.dir, tmp_fs_usage, &fs));
    s = &bf.iface;
}

void tearDown(void)
{
    blob_fs_deinit(&bf);
    tmp_fs_remove_dir(fs.dir);
}

static void put(const char *name, const char *text)
{
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, name, 0, text, strlen(text)));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->flush(s->ctx, name));
}

static void test_init_checks_the_directory_name(void)
{
    blob_fs_t b;
    TEST_ASSERT_FALSE(blob_fs_init(&b, "", NULL, NULL));
    TEST_ASSERT_FALSE(blob_fs_init(&b, "/assets/", NULL, NULL));
    TEST_ASSERT_FALSE(blob_fs_init(&b, "/a/directory/name/that/is/far/too/long", NULL, NULL));
    TEST_ASSERT_TRUE(blob_fs_init(&b, "/assets", NULL, NULL));
}

static void test_write_stat_read(void)
{
    uint32_t size = 0;
    char buf[8] = "";
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->stat(s->ctx, "3_a", &size));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "3_a", 0, "hello", 5));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "3_a", 5, " you", 4)); /* append to the open file */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->stat(s->ctx, "3_a", &size));
    TEST_ASSERT_EQUAL_UINT32(9, size);
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->read(s->ctx, "3_a", 4, buf, 5));
    TEST_ASSERT_EQUAL_STRING_LEN("o you", buf, 5);
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->read(s->ctx, "3_a", 6, buf, 4)); /* past the end */
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->read(s->ctx, "3_b", 0, buf, 1));
}

/* The board's upload: many chunks to one temp file (the phone's first ack only comes after ack_window bytes,
 * so a failure at chunk 2 looks like a failed first chunk). Seen on the ostb board as XFER_ACK INTERNAL. */
static void test_chunks_to_an_open_file_need_no_fstat(void)
{
    uint8_t chunk[200];
    fs.total = 100000; /* the icon must fit: this test is about the open file, not about space */
    memset(chunk, 0x5A, sizeof(chunk));
    for (uint32_t i = 0; i < 15; i++) { /* 3000 bytes in 200-byte chunks, as a 3088-byte icon would arrive */
        TEST_ASSERT_EQUAL_INT_MESSAGE(BLOB_OK, s->write(s->ctx, "3_a~", i * 200, chunk, sizeof(chunk)), "chunk");
    }
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "3_a~", 3000, chunk, 88));
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->write(s->ctx, "3_a~", 3100, chunk, 1)); /* a gap past the open file's end */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "3_a~", 100, chunk, 50));  /* a rewrite inside it */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->flush(s->ctx, "3_a~"));
    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->stat(s->ctx, "3_a~", &size));
    TEST_ASSERT_EQUAL_UINT32(3088, size);
}

static void test_overwrite_inside_and_gap_is_refused(void)
{
    put("3_a", "abcdef");
    char buf[8] = "";
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "3_a", 2, "XY", 2)); /* resume: rewrites a range */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->read(s->ctx, "3_a", 0, buf, 6));
    TEST_ASSERT_EQUAL_STRING_LEN("abXYef", buf, 6);
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->write(s->ctx, "3_a", 7, "Z", 1)); /* a gap */
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->write(s->ctx, "3_new", 1, "Z", 1));
    uint32_t size;
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->stat(s->ctx, "3_new", &size)); /* and no file was made */
}

static void test_no_space_writes_nothing(void)
{
    char big[1001];
    memset(big, 'x', sizeof(big));
    uint32_t size;
    TEST_ASSERT_EQUAL_INT(BLOB_NO_SPACE, s->write(s->ctx, "3_a", 0, big, 1001));
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->stat(s->ctx, "3_a", &size));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "3_a", 0, big, 900));
    TEST_ASSERT_EQUAL_INT(BLOB_NO_SPACE, s->write(s->ctx, "3_a", 900, big, 101)); /* growing past the end */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "3_a", 800, big, 200));      /* 100 more bytes fit */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->stat(s->ctx, "3_a", &size));
    TEST_ASSERT_EQUAL_UINT32(1000, size);
    uint32_t total, free_bytes;
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->usage(s->ctx, &total, &free_bytes));
    TEST_ASSERT_EQUAL_UINT32(1000, total);
    TEST_ASSERT_EQUAL_UINT32(0, free_bytes);
}

static void test_rename_replaces_atomically(void)
{
    put("3_a~", "new");
    put("3_a", "old content");
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "3_a~", 3, "!", 1)); /* the temp file is still open */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->rename(s->ctx, "3_a~", "3_a"));
    char buf[8] = "";
    uint32_t size;
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->stat(s->ctx, "3_a", &size));
    TEST_ASSERT_EQUAL_UINT32(4, size);
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->read(s->ctx, "3_a", 0, buf, 4));
    TEST_ASSERT_EQUAL_STRING_LEN("new!", buf, 4);
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->stat(s->ctx, "3_a~", &size));
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->rename(s->ctx, "3_a~", "3_a"));
}

static void test_remove_is_idempotent(void)
{
    put("3_a", "x");
    uint32_t size;
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->remove(s->ctx, "3_a"));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->remove(s->ctx, "3_a"));
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->stat(s->ctx, "3_a", &size));
}

static void test_flush_makes_the_file_readable_by_others(void)
{
    char path[BLOB_FS_DIR_MAX + 16];
    snprintf(path, sizeof(path), "%s/3_a", fs.dir);
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "3_a", 0, "abc", 3));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bf.fd); /* kept open for the next chunk */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->flush(s->ctx, "3_other")); /* another file's flush leaves it open */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bf.fd);
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->flush(s->ctx, "3_a"));
    TEST_ASSERT_EQUAL_INT(-1, bf.fd);
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(path, &st));
    TEST_ASSERT_EQUAL_INT(3, (int)st.st_size);
}

static void test_writing_another_file_closes_the_first(void)
{
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "3_a", 0, "abc", 3));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "3_b", 0, "de", 2));
    TEST_ASSERT_EQUAL_STRING("3_b", bf.open_name);
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "3_a", 3, "def", 3)); /* and back: reopened, appended */
    uint32_t size;
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->stat(s->ctx, "3_a", &size));
    TEST_ASSERT_EQUAL_UINT32(6, size);
}

static void test_list_skips_directories_and_stays_stable(void)
{
    put("3_a", "1");
    put("3_b", "22");
    put("2_c", "333");
    char sub[BLOB_FS_DIR_MAX + 16];
    snprintf(sub, sizeof(sub), "%s/diag", fs.dir); /* the battery log's directory is not an asset */
    TEST_ASSERT_EQUAL_INT(0, mkdir(sub, 0755));

    uint32_t sizes[4], n = 0, size;
    char names[4][BLOB_NAME_MAX + 1], name[BLOB_NAME_MAX + 1];
    while (s->list(s->ctx, n, name, sizeof(name), &size) == BLOB_OK && n < 4) {
        strcpy(names[n], name);
        sizes[n++] = size;
    }
    TEST_ASSERT_EQUAL_UINT32(3, n);
    for (uint32_t i = 0; i < n; i++) { /* the same order the second time, and sizes that match the names */
        TEST_ASSERT_EQUAL_INT(BLOB_OK, s->list(s->ctx, i, name, sizeof(name), &size));
        TEST_ASSERT_EQUAL_STRING(names[i], name);
        TEST_ASSERT_EQUAL_UINT32(sizes[i], size);
        TEST_ASSERT_NOT_EQUAL(0, strcmp(name, "diag"));
    }
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->list(s->ctx, 3, name, sizeof(name), &size));
    char tiny[3];
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->list(s->ctx, 0, tiny, sizeof(tiny), &size)); /* name does not fit */
}

static void test_a_walk_of_the_listing_is_one_pass(void)
{
    enum { N = 30 };
    char names[N][BLOB_NAME_MAX + 1];
    for (int i = 0; i < N; i++) {
        snprintf(names[i], sizeof(names[i]), "5_%02d", i);
        put(names[i], "abc");
    }
    char sub[BLOB_FS_DIR_MAX + 16];
    snprintf(sub, sizeof(sub), "%s/diag", fs.dir);
    TEST_ASSERT_EQUAL_INT(0, mkdir(sub, 0755));

    char name[BLOB_NAME_MAX + 1], first[N][BLOB_NAME_MAX + 1];
    uint32_t size, n = 0;
    stat_calls = 0;
    while (s->list(s->ctx, n, name, sizeof(name), &size) == BLOB_OK && n < N + 1) {
        TEST_ASSERT_EQUAL_UINT32(3, size);
        strcpy(first[n++], name);
    }
    TEST_ASSERT_EQUAL_UINT32(N, n);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(N + 1, stat_calls); /* the size of each file, once (not N * N / 2) */

    /* Asking out of order starts over and gives the same names. */
    for (int i = N - 1; i >= 0; i -= 7) {
        TEST_ASSERT_EQUAL_INT(BLOB_OK, s->list(s->ctx, (uint32_t)i, name, sizeof(name), &size));
        TEST_ASSERT_EQUAL_STRING(first[i], name);
    }
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->list(s->ctx, 3, name, sizeof(name), &size)); /* and skipping ahead */
    TEST_ASSERT_EQUAL_STRING(first[3], name);
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->list(s->ctx, 20, name, sizeof(name), &size));
    TEST_ASSERT_EQUAL_STRING(first[20], name);
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->list(s->ctx, 21, name, sizeof(name), &size));
    TEST_ASSERT_EQUAL_STRING(first[21], name);
}

static void test_removing_while_walking_shifts_the_listing(void)
{
    /* What xfer_init does: remove the entry at i, then ask for i again without moving on. */
    put("5_a", "1");
    put("5_b~", "2");
    put("5_c", "3");
    put("5_d~", "4");
    char name[BLOB_NAME_MAX + 1];
    uint32_t size, i = 0, kept = 0, removed = 0;
    while (s->list(s->ctx, i, name, sizeof(name), &size) == BLOB_OK) {
        if (name[strlen(name) - 1] == '~') {
            TEST_ASSERT_EQUAL_INT(BLOB_OK, s->remove(s->ctx, name));
            removed++;
            continue;
        }
        kept++;
        i++;
    }
    TEST_ASSERT_EQUAL_UINT32(2, removed);
    TEST_ASSERT_EQUAL_UINT32(2, kept);
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->stat(s->ctx, "5_b~", &size));
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->stat(s->ctx, "5_d~", &size));
}

static void test_a_write_or_rename_between_list_calls_is_seen(void)
{
    put("5_a", "1");
    char name[BLOB_NAME_MAX + 1];
    uint32_t size;
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->list(s->ctx, 0, name, sizeof(name), &size));
    put("5_b", "22"); /* a new file while a listing is open */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->list(s->ctx, 1, name, sizeof(name), &size));
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, s->list(s->ctx, 2, name, sizeof(name), &size));

    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->list(s->ctx, 0, name, sizeof(name), &size));
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->rename(s->ctx, "5_a", "5_z"));
    uint32_t n = 0;
    while (s->list(s->ctx, n, name, sizeof(name), &size) == BLOB_OK && n < 4) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp(name, "5_a"));
        n++;
    }
    TEST_ASSERT_EQUAL_UINT32(2, n);
}

static void test_list_sees_unsynced_sizes(void)
{
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->write(s->ctx, "3_a", 0, "abcd", 4)); /* still open */
    char name[BLOB_NAME_MAX + 1];
    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(BLOB_OK, s->list(s->ctx, 0, name, sizeof(name), &size));
    TEST_ASSERT_EQUAL_UINT32(4, size);
}

static void test_names_cannot_leave_the_directory(void)
{
    uint32_t size;
    char buf[1];
    const char *bad[] = {"", ".", "..", "../x", "a/b", ".hidden", "/etc/passwd"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->write(s->ctx, bad[i], 0, "x", 1));
        TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->stat(s->ctx, bad[i], &size));
        TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->read(s->ctx, bad[i], 0, buf, 1));
        TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->remove(s->ctx, bad[i]));
        TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->rename(s->ctx, bad[i], "3_a"));
    }
    char long_name[BLOB_NAME_MAX + 2];
    memset(long_name, 'a', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, s->write(s->ctx, long_name, 0, "x", 1));
}

static char traced_step[16];
static int traced_err, traced_calls;

static void trace_to_test(void *ctx, const char *step, int err)
{
    (void)ctx;
    snprintf(traced_step, sizeof(traced_step), "%s", step);
    traced_err = err;
    traced_calls++;
}

static blob_status_t usage_fails(void *ctx, uint32_t *total, uint32_t *free_bytes)
{
    (void)ctx;
    (void)total;
    (void)free_bytes;
    return BLOB_ERR;
}

static void test_a_failed_write_says_which_step_failed(void)
{
    blob_fs_t b;
    traced_calls = 0;
    TEST_ASSERT_TRUE(blob_fs_init(&b, "/tmp/epager_no_such_dir_xyz", tmp_fs_usage, &fs));
    blob_fs_set_trace(&b, trace_to_test, NULL);
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, b.iface.write(b.iface.ctx, "3_a", 0, "x", 1));
    TEST_ASSERT_EQUAL_STRING("open", traced_step);
    TEST_ASSERT_EQUAL_INT(ENOENT, traced_err);
    TEST_ASSERT_EQUAL_INT(1, traced_calls);

    TEST_ASSERT_TRUE(blob_fs_init(&b, fs.dir, usage_fails, NULL));
    blob_fs_set_trace(&b, trace_to_test, NULL);
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, b.iface.write(b.iface.ctx, "3_a", 0, "x", 1));
    TEST_ASSERT_EQUAL_STRING("usage", traced_step);

    TEST_ASSERT_EQUAL_INT(BLOB_ERR, b.iface.write(b.iface.ctx, "../x", 0, "x", 1)); /* a bad name is not a storage fault */
    TEST_ASSERT_EQUAL_INT(2, traced_calls);

    TEST_ASSERT_TRUE(blob_fs_init(&b, fs.dir, tmp_fs_usage, &fs)); /* no trace set: nothing to call */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, b.iface.write(b.iface.ctx, "3_a", 0, "x", 1));
    TEST_ASSERT_EQUAL_INT(2, traced_calls);
    TEST_ASSERT_EQUAL_INT(BLOB_OK, b.iface.flush(b.iface.ctx, "3_a"));
}

static void test_a_missing_directory_is_an_error_not_a_crash(void)
{
    blob_fs_t b;
    TEST_ASSERT_TRUE(blob_fs_init(&b, "/tmp/epager_no_such_dir_xyz", tmp_fs_usage, &fs));
    uint32_t size;
    char name[BLOB_NAME_MAX + 1];
    TEST_ASSERT_EQUAL_INT(BLOB_NOT_FOUND, b.iface.stat(b.iface.ctx, "3_a", &size));
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, b.iface.list(b.iface.ctx, 0, name, sizeof(name), &size));
    TEST_ASSERT_EQUAL_INT(BLOB_ERR, b.iface.write(b.iface.ctx, "3_a", 0, "x", 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_checks_the_directory_name);
    RUN_TEST(test_write_stat_read);
    RUN_TEST(test_chunks_to_an_open_file_need_no_fstat);
    RUN_TEST(test_overwrite_inside_and_gap_is_refused);
    RUN_TEST(test_no_space_writes_nothing);
    RUN_TEST(test_rename_replaces_atomically);
    RUN_TEST(test_remove_is_idempotent);
    RUN_TEST(test_flush_makes_the_file_readable_by_others);
    RUN_TEST(test_writing_another_file_closes_the_first);
    RUN_TEST(test_list_skips_directories_and_stays_stable);
    RUN_TEST(test_a_walk_of_the_listing_is_one_pass);
    RUN_TEST(test_removing_while_walking_shifts_the_listing);
    RUN_TEST(test_a_write_or_rename_between_list_calls_is_seen);
    RUN_TEST(test_list_sees_unsynced_sizes);
    RUN_TEST(test_names_cannot_leave_the_directory);
    RUN_TEST(test_a_failed_write_says_which_step_failed);
    RUN_TEST(test_a_missing_directory_is_an_error_not_a_crash);
    return UNITY_END();
}
