/* blob_storage over a directory of files (see blob_fs.h). */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE /* DT_REG on glibc */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "epager/blob_fs.h"

#define PATH_MAX_LEN (BLOB_FS_DIR_MAX + 1u + BLOB_NAME_MAX + 1u)

/* Names are flat ASCII; nothing that could leave the directory. */
static bool path_of(const blob_fs_t *b, const char *name, char out[PATH_MAX_LEN])
{
    const size_t n = strlen(name);
    if (n == 0 || n > BLOB_NAME_MAX || name[0] == '.' || strchr(name, '/') != NULL) {
        return false;
    }
    const size_t d = strlen(b->dir);
    memcpy(out, b->dir, d);
    out[d] = '/';
    memcpy(out + d + 1, name, n + 1);
    return true;
}

static blob_status_t trace_fail(const blob_fs_t *b, const char *step, int err, blob_status_t res)
{
    if (b->trace != NULL) {
        b->trace(b->trace_ctx, step, err);
    }
    return res;
}

/* Forget the directory kept open between list() calls (see fs_list). */
static void close_list(blob_fs_t *b)
{
    if (b->list_dir != NULL) {
        closedir(b->list_dir);
        b->list_dir = NULL;
    }
    b->list_next = 0;
}

/* Sync and close the file kept open for writing. */
static blob_status_t close_open(blob_fs_t *b)
{
    if (b->fd < 0) {
        return BLOB_OK;
    }
    const bool ok = fsync(b->fd) == 0;
    int err = ok ? 0 : errno;
    const bool closed = close(b->fd) == 0;
    err = err != 0 || closed ? err : errno;
    b->fd = -1;
    b->open_name[0] = '\0';
    b->open_size = 0;
    return ok && closed ? BLOB_OK : trace_fail(b, "sync", err, BLOB_ERR);
}

static blob_status_t fs_stat(void *ctx, const char *name, uint32_t *size)
{
    blob_fs_t *b = ctx;
    char path[PATH_MAX_LEN];
    struct stat st;
    if (!path_of(b, name, path) || close_open(b) != BLOB_OK) {
        return BLOB_ERR;
    }
    if (stat(path, &st) != 0) {
        return errno == ENOENT ? BLOB_NOT_FOUND : BLOB_ERR;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) {
        return BLOB_ERR;
    }
    *size = (uint32_t)st.st_size;
    return BLOB_OK;
}

static blob_status_t fs_read(void *ctx, const char *name, uint32_t off, void *buf, size_t len)
{
    blob_fs_t *b = ctx;
    char path[PATH_MAX_LEN];
    if (!path_of(b, name, path) || close_open(b) != BLOB_OK) {
        return BLOB_ERR;
    }
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return errno == ENOENT ? BLOB_NOT_FOUND : BLOB_ERR;
    }
    blob_status_t st = lseek(fd, (off_t)off, SEEK_SET) == (off_t)off ? BLOB_OK : BLOB_ERR;
    uint8_t *p = buf;
    while (st == BLOB_OK && len > 0) {
        const ssize_t n = read(fd, p, len);
        if (n <= 0) { /* a short file is an error: exactly len bytes or nothing */
            st = BLOB_ERR;
            break;
        }
        p += n;
        len -= (size_t)n;
    }
    close(fd);
    return st;
}

static blob_status_t fs_usage(void *ctx, uint32_t *total, uint32_t *free_bytes)
{
    blob_fs_t *b = ctx;
    if (b->usage == NULL || close_open(b) != BLOB_OK) {
        return BLOB_ERR;
    }
    return b->usage(b->usage_ctx, total, free_bytes);
}

static blob_status_t fs_write(void *ctx, const char *name, uint32_t off, const void *data, size_t len)
{
    blob_fs_t *b = ctx;
    char path[PATH_MAX_LEN];
    if (!path_of(b, name, path)) {
        return BLOB_ERR;
    }
    close_list(b);
    if (b->fd >= 0 && strcmp(b->open_name, name) != 0 && close_open(b) != BLOB_OK) {
        return BLOB_ERR;
    }
    /* The current size: tracked for the open file (it holds bytes not yet synced), from the directory otherwise. */
    uint64_t cur = b->open_size;
    if (b->fd < 0) {
        struct stat st;
        cur = 0;
        if (stat(path, &st) == 0) {
            cur = st.st_size < 0 ? 0 : (uint64_t)st.st_size;
        } else if (errno != ENOENT) {
            return trace_fail(b, "stat", errno, BLOB_ERR);
        }
    }
    if (off > cur) {
        return BLOB_ERR;
    }
    if (off + len > cur) { /* growing: nothing is written, and no file made, when it cannot fit */
        uint32_t total, free_bytes;
        if (b->usage == NULL || b->usage(b->usage_ctx, &total, &free_bytes) != BLOB_OK) {
            return trace_fail(b, "usage", 0, BLOB_ERR);
        }
        if (off + len - cur > free_bytes) {
            return BLOB_NO_SPACE;
        }
    }
    if (b->fd < 0) {
        b->fd = open(path, O_RDWR | O_CREAT, 0644);
        if (b->fd < 0) {
            return trace_fail(b, "open", errno, errno == ENOSPC ? BLOB_NO_SPACE : BLOB_ERR);
        }
        strcpy(b->open_name, name);
        b->open_size = cur;
    }
    const char *step = "seek";
    blob_status_t res = BLOB_OK;
    int err = 0;
    if (lseek(b->fd, (off_t)off, SEEK_SET) != (off_t)off) {
        res = BLOB_ERR;
        err = errno;
    }
    const uint8_t *p = data;
    size_t left = len;
    while (res == BLOB_OK && left > 0) {
        step = "write";
        const ssize_t n = write(b->fd, p, left);
        if (n <= 0) {
            err = errno;
            res = err == ENOSPC ? BLOB_NO_SPACE : BLOB_ERR;
            break;
        }
        p += n;
        left -= (size_t)n;
    }
    if (res == BLOB_OK) {
        b->open_size = off + len > b->open_size ? off + len : b->open_size;
    } else {
        close_open(b); /* never keep a file open after a failed write */
        trace_fail(b, step, err, res);
    }
    return res;
}

static blob_status_t fs_flush(void *ctx, const char *name)
{
    blob_fs_t *b = ctx;
    /* Closing syncs; a file that is not open was synced when it was closed. */
    return b->fd >= 0 && strcmp(b->open_name, name) == 0 ? close_open(b) : BLOB_OK;
}

static blob_status_t fs_rename(void *ctx, const char *from, const char *to)
{
    blob_fs_t *b = ctx;
    char src[PATH_MAX_LEN], dst[PATH_MAX_LEN];
    if (!path_of(b, from, src) || !path_of(b, to, dst) || close_open(b) != BLOB_OK) {
        return BLOB_ERR;
    }
    close_list(b);
    if (rename(src, dst) != 0) { /* LittleFS replaces the target atomically */
        return errno == ENOENT ? BLOB_NOT_FOUND : BLOB_ERR;
    }
    return BLOB_OK;
}

static blob_status_t fs_remove(void *ctx, const char *name)
{
    blob_fs_t *b = ctx;
    char path[PATH_MAX_LEN];
    if (!path_of(b, name, path) || close_open(b) != BLOB_OK) {
        return BLOB_ERR;
    }
    close_list(b);
    return unlink(path) == 0 || errno == ENOENT ? BLOB_OK : BLOB_ERR;
}

/* Whether a directory entry is a regular file with a name of ours. A name the directory already types needs no
 * stat() unless the caller wants the size: on LittleFS every stat() walks the directory's metadata log. */
static bool entry_is_file(const blob_fs_t *b, const struct dirent *e, bool want_size, struct stat *st)
{
    char path[PATH_MAX_LEN];
    if (e->d_name[0] == '.' || !path_of(b, e->d_name, path)) {
        return false; /* ".", "..", names that are not ours */
    }
    if (e->d_type != DT_REG && e->d_type != DT_UNKNOWN) {
        return false; /* subdirectories */
    }
    if (e->d_type == DT_REG && !want_size) {
        return true;
    }
    return stat(path, st) == 0 && S_ISREG(st->st_mode);
}

/* The callers walk the indexes 0, 1, 2, ...: the directory stays open between the calls, so a walk is one pass
 * over the directory, not one pass per index (that took 10 s of boot with 40 files on LittleFS). Any other index
 * starts over, and so does every change of the directory. */
static blob_status_t fs_list(void *ctx, uint32_t index, char *name, size_t cap, uint32_t *size)
{
    blob_fs_t *b = ctx;
    if (close_open(b) != BLOB_OK) {
        return BLOB_ERR;
    }
    if (b->list_dir == NULL || index != b->list_next) {
        close_list(b);
        b->list_dir = opendir(b->dir);
        if (b->list_dir == NULL) {
            return BLOB_ERR;
        }
    }
    blob_status_t res = BLOB_NOT_FOUND;
    const struct dirent *e;
    while ((e = readdir(b->list_dir)) != NULL) {
        struct stat st;
        if (b->list_next < index) { /* starting over: count the files, their sizes are not needed */
            if (entry_is_file(b, e, false, &st)) {
                b->list_next++;
            }
            continue;
        }
        if (!entry_is_file(b, e, true, &st)) {
            continue;
        }
        const size_t n = strlen(e->d_name);
        if (n >= cap || st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) {
            res = BLOB_ERR;
            break;
        }
        memcpy(name, e->d_name, n + 1);
        *size = (uint32_t)st.st_size;
        b->list_next++;
        return BLOB_OK; /* the directory stays open for the next index */
    }
    close_list(b);
    return res;
}

void blob_fs_deinit(blob_fs_t *b)
{
    close_list(b);
    (void)close_open(b);
}

void blob_fs_set_trace(blob_fs_t *b, blob_fs_trace_fn fn, void *ctx)
{
    b->trace = fn;
    b->trace_ctx = ctx;
}

bool blob_fs_init(blob_fs_t *b, const char *dir, blob_fs_usage_fn usage, void *usage_ctx)
{
    const size_t n = strlen(dir);
    if (n == 0 || n > BLOB_FS_DIR_MAX || dir[n - 1] == '/') {
        return false;
    }
    memset(b, 0, sizeof(*b));
    memcpy(b->dir, dir, n + 1);
    b->usage = usage;
    b->usage_ctx = usage_ctx;
    b->fd = -1;
    b->iface = (blob_storage_t){.stat = fs_stat, .read = fs_read, .write = fs_write, .flush = fs_flush,
                                .rename = fs_rename, .remove = fs_remove, .list = fs_list, .usage = fs_usage,
                                .ctx = b};
    return true;
}
