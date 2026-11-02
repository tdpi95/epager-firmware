/*
 * Key-value storage interface used by the portable core (config, seen apps, transfer journal,
 * OTA record). On the board it maps to one NVS namespace per instance; on the host to kv_mem.
 * Portable C11.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KV_KEY_MAX 15u /* NVS key length limit */

typedef enum {
    KV_OK = 0,
    KV_NOT_FOUND,
    KV_TOO_SMALL, /* get: value larger than the buffer */
    KV_ERR,       /* I/O error or out of space; the stored value is unchanged */
} kv_status_t;

typedef struct {
    /* Read the whole value. *len = its length on KV_OK. */
    kv_status_t (*get)(void *ctx, const char *key, void *buf, size_t cap, size_t *len);
    /* Replace the value atomically: after a power loss either the old or the new value is stored. */
    kv_status_t (*set)(void *ctx, const char *key, const void *data, size_t len);
    /* Remove the key; KV_OK if it did not exist. */
    kv_status_t (*erase)(void *ctx, const char *key);
    void *ctx;
} kv_storage_t;

/* ---------------------------------------------------------------- in-memory fake (host, fake board) */

#define KV_MEM_MAX_ENTRIES 160u
#define KV_MEM_MAX_VALUE 256u

typedef struct {
    bool used;
    char key[KV_KEY_MAX + 1];
    uint16_t len;
    uint8_t val[KV_MEM_MAX_VALUE];
} kv_mem_entry_t;

typedef struct {
    kv_storage_t iface;
    kv_mem_entry_t e[KV_MEM_MAX_ENTRIES];
    /* Power-loss simulation, shared with the other fakes: NULL = unlimited. Each set/erase
     * consumes one unit; when it is 0 the operation fails with KV_ERR and changes nothing. */
    int32_t *power;
    uint32_t mutations; /* successful set/erase calls */
} kv_mem_t;

/* Empty store. The contents survive a simulated reboot: re-initialise the users, not the store. */
void kv_mem_init(kv_mem_t *m);
size_t kv_mem_count(const kv_mem_t *m);

#ifdef __cplusplus
}
#endif
