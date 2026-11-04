/*
 * Board configuration (protocol.md 5): typed keys from the generated registry tables, batch
 * all-or-nothing writes, revisions, change events, defaults and migration by `since`.
 * Persisted as one atomic kv record. Portable C11; single-threaded (the core task owns it).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/kv_storage.h"
#include "epager/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_MAX_BATCH 64u      /* CONFIG_SET entries (protocol.md 5) */
#define CFG_KV_KEY "cfg"

/* Called after a committed change: new revision and the changed keys (ascending id; none for a
 * policy-only change). Build the CONFIG_CHANGED event with cfg_write_changed_event(). */
typedef void (*cfg_change_fn)(void *ctx, uint32_t revision, const uint16_t *keys, size_t n_keys);

typedef struct {
    const kv_storage_t *kv;
    uint8_t variant;                         /* PROTO_VARIANT_*: decides which keys exist */
    cfg_change_fn on_change;                 /* optional */
    void (*set_time)(void *ctx, uint32_t utc_s); /* TIME_SYNC clock hook; optional */
    void *ctx;
} cfg_params_t;

typedef struct {
    int32_t i;                               /* integer and bool keys */
    uint8_t len;                             /* str keys */
    uint8_t s[PROTO_CFG_STR_MAX_LEN];
} cfg_value_t;

/* Largest stored record (cfg_store.c): header, one entry per key, CRC. */
#define CFG_BLOB_MAX (12u + PROTO_CFG_KEY_COUNT * (3u + (PROTO_CFG_STR_MAX_LEN > 4u ? PROTO_CFG_STR_MAX_LEN : 4u)) + 4u)

typedef struct {
    cfg_params_t p;
    uint32_t revision;
    cfg_value_t v[PROTO_CFG_KEY_COUNT];      /* indexed like proto_cfg_keys */
    /* scratch for set/reset and the stored record: kept off the caller's stack (single-threaded) */
    cfg_value_t nv[PROTO_CFG_KEY_COUNT];
    uint8_t blob[CFG_BLOB_MAX];
} cfg_store_t;

typedef struct {
    uint16_t key;
    const uint8_t *val;                      /* wire encoding (protocol.md 2.3) */
    size_t len;
} cfg_entry_t;

/* Load from storage. Missing, corrupt or invalid stored values fall back to defaults; keys added
 * after the stored record's contract version (`since`) start at their defaults. */
void cfg_init(cfg_store_t *c, const cfg_params_t *p);

uint32_t cfg_revision(const cfg_store_t *c);
/* Integer/bool value; the default for keys not on this variant; 0 for unknown keys. */
int32_t cfg_get_int(const cfg_store_t *c, uint16_t key);
/* String or bytes value (not NUL-terminated); length 0 for unknown keys. */
size_t cfg_get_str(const cfg_store_t *c, uint16_t key, const uint8_t **s);
/* Wire encoding of a key's value into out[PROTO_CFG_STR_MAX_LEN]. False if the key does not exist on this variant. */
bool cfg_encode_value(const cfg_store_t *c, uint16_t key, uint8_t *out, size_t *len);

/* CONFIG_SET semantics (protocol.md 5). key_status[n] receives the per-entry status when the
 * result is OK. Returns OK, the first failing key's status, REVISION_CONFLICT, TOO_LARGE (batch
 * over CFG_MAX_BATCH) or INTERNAL (storage failure; nothing changed). */
proto_status_t cfg_set(cfg_store_t *c, const cfg_entry_t *e, size_t n, const uint32_t *expected_revision,
                       proto_status_t *key_status);
/* All keys to defaults (revision/event rules as cfg_set). */
proto_status_t cfg_reset(cfg_store_t *c);
/* Policy change elsewhere (APPS_*): revision + 1 and an event without keys. */
proto_status_t cfg_bump_revision(cfg_store_t *c);

/* Message adapter for CONFIG_GET, CONFIG_SET, CONFIG_RESET and TIME_SYNC (already validated by
 * proto_msg_parse). Writes the complete response into w. Returns false for other types. */
bool cfg_handle(cfg_store_t *c, const proto_msg_t *req, proto_writer_t *w);
void cfg_write_changed_event(proto_writer_t *w, uint32_t revision, const uint16_t *keys, size_t n_keys);

#ifdef __cplusplus
}
#endif
