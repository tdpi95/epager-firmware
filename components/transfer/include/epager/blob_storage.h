/*
 * Flat file storage for assets (LittleFS on the board, blob_mem on the host). Portable C11.
 * Names are short ASCII ("<type>_<id>", plus '~' for temporary and '@' for version files).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLOB_NAME_MAX 40u

typedef enum {
    BLOB_OK = 0,
    BLOB_NOT_FOUND,
    BLOB_NO_SPACE,
    BLOB_ERR,
} blob_status_t;

typedef struct {
    blob_status_t (*stat)(void *ctx, const char *name, uint32_t *size);
    /* Exactly len bytes at off, else BLOB_ERR. */
    blob_status_t (*read)(void *ctx, const char *name, uint32_t off, void *buf, size_t len);
    /* Creates the file if missing; off must be <= its size. Nothing is written on BLOB_NO_SPACE. */
    blob_status_t (*write)(void *ctx, const char *name, uint32_t off, const void *data, size_t len);
    /* Make written data durable (called before the transfer journal records an offset). */
    blob_status_t (*flush)(void *ctx, const char *name);
    /* Atomic: afterwards `to` has either its old content or all of `from`'s. Replaces `to`. */
    blob_status_t (*rename)(void *ctx, const char *from, const char *to);
    /* BLOB_OK if the file did not exist. */
    blob_status_t (*remove)(void *ctx, const char *name);
    /* index-th file in a stable order (while nothing changes); BLOB_NOT_FOUND past the end. */
    blob_status_t (*list)(void *ctx, uint32_t index, char *name, size_t cap, uint32_t *size);
    blob_status_t (*usage)(void *ctx, uint32_t *total, uint32_t *free_bytes);
    void *ctx;
} blob_storage_t;

/* ---------------------------------------------------------------- in-memory fake (host, fake board) */

#define BLOB_MEM_MAX_FILES 64u

typedef struct {
    bool used;
    char name[BLOB_NAME_MAX + 1];
    uint32_t size;
    uint8_t *data; /* heap: this fake is a host test double, never used in firmware */
} blob_mem_file_t;

typedef struct {
    blob_storage_t iface;
    blob_mem_file_t f[BLOB_MEM_MAX_FILES];
    uint32_t total;   /* free = total - sum of file sizes (the reference accounting of transfer.json) */
    int32_t *power;   /* shared power budget (see kv_mem): each write/rename/remove consumes one unit */
} blob_mem_t;

void blob_mem_init(blob_mem_t *m, uint32_t total);
void blob_mem_free(blob_mem_t *m);
/* Test helper: the file's bytes, or NULL. */
const uint8_t *blob_mem_data(const blob_mem_t *m, const char *name, uint32_t *size);

#ifdef __cplusplus
}
#endif
