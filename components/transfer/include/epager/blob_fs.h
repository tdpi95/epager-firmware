/*
 * blob_storage over a directory of plain files (LittleFS mounted at /assets on the board, a temp
 * directory in host tests). POSIX calls only: no ESP-IDF header, so it is built and tested on the host.
 * One file stays open between writes (a transfer appends chunk after chunk); any other call, and flush,
 * syncs and closes it first, so every reader sees the bytes and LittleFS can rename it.
 * The open file's size is tracked here, never asked of fstat(): esp_littlefs answers fstat() from the directory
 * entry, which a file created or grown since its last sync does not have yet (ENOENT or an old size).
 * list() keeps its directory open for the next index (a walk of n files is one pass, not n); every write, rename
 * and remove closes it, and any other index starts over.
 * Only regular files are listed: subdirectories of the mount (the battery log's) are not assets.
 */
#pragma once

#include <stdint.h>

#include "epager/blob_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLOB_FS_DIR_MAX 32u

/* Capacity of the file system: the board asks the LittleFS partition, a test returns a number. */
typedef blob_status_t (*blob_fs_usage_fn)(void *ctx, uint32_t *total, uint32_t *free_bytes);

/* Called when a write fails: the step ("usage", "stat", "open", "seek", "write", "sync") and the errno behind it
 * (0 if the step has none). Names and ids of assets are not passed: the board logs this line. */
typedef void (*blob_fs_trace_fn)(void *ctx, const char *step, int err);

typedef struct {
    blob_storage_t iface;
    char dir[BLOB_FS_DIR_MAX + 1]; /* no trailing slash */
    blob_fs_usage_fn usage;
    void *usage_ctx;
    blob_fs_trace_fn trace;
    void *trace_ctx;
    int fd;                        /* the file kept open for writing, -1 if none */
    uint64_t open_size;            /* its size, with the bytes not yet synced */
    char open_name[BLOB_NAME_MAX + 1];
    void *list_dir;                /* DIR * kept open between list() calls with consecutive indexes, NULL if none */
    uint32_t list_next;            /* the index the next call continues with */
} blob_fs_t;

/* False if dir is empty, too long or has a trailing slash. */
bool blob_fs_init(blob_fs_t *b, const char *dir, blob_fs_usage_fn usage, void *usage_ctx);

/* Release the directory and the file kept open (a board never calls it; tests do, between cases). */
void blob_fs_deinit(blob_fs_t *b);

/* Optional: report the step and errno of a failed write (fn NULL turns it off). */
void blob_fs_set_trace(blob_fs_t *b, blob_fs_trace_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif
