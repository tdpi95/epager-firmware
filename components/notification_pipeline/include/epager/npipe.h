/*
 * Notification pipeline (protocol.md 6): ANCS / Android input -> NotificationEvent normalisation ->
 * filters -> RAM store -> events for the UI. Also tracks seen apps and their policies (protocol.md 5).
 * Notifications live in RAM only; seen apps are persisted through kv_storage.
 * Portable C11; single-threaded (the core task owns it). No logging: content never leaves here
 * except through the callbacks.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/cfg_store.h"
#include "epager/kv_storage.h"
#include "epager/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Field limits in bytes, from the registry (Android maxima; ANCS requests are smaller). */
#define NPIPE_KEY_MAX 64u
#define NPIPE_APP_ID_MAX 64u
#define NPIPE_APP_NAME_MAX 64u
#define NPIPE_TITLE_MAX 256u
#define NPIPE_SUBTITLE_MAX 256u
#define NPIPE_BODY_MAX 1024u

typedef enum { NPIPE_ADDED = 0, NPIPE_MODIFIED = 1, NPIPE_REMOVED = 2 } npipe_kind_t;

/* The board-internal NotificationEvent. Text is display-ready (protocol.md 8), NUL-terminated. */
typedef struct {
    uint32_t id;          /* board-local, never 0 */
    uint8_t source;       /* PROTO_NOTIF_SOURCE_*; UI code must not depend on it */
    uint8_t kind;         /* npipe_kind_t of the last change */
    uint8_t category;     /* PROTO_CATEGORY_* (unknown values mapped to other) */
    uint8_t priority;     /* PROTO_PRIORITY_* */
    uint8_t flags;        /* PROTO_NOTIF_FLAGS_* */
    bool shown;           /* passed the filters (protocol.md 6.3) */
    uint32_t timestamp;   /* UTC seconds, 0 = unknown */
    uint16_t app_id_len, app_name_len, title_len, subtitle_len, body_len;
    char app_id[NPIPE_APP_ID_MAX + 1]; /* valid UTF-8, not normalised */
    char app_name[NPIPE_APP_NAME_MAX + 1];
    char title[NPIPE_TITLE_MAX + 1];
    char subtitle[NPIPE_SUBTITLE_MAX + 1];
    char body[NPIPE_BODY_MAX + 1];
} npipe_notif_t;

/* Store slot (allocate an array of these, e.g. in PSRAM). */
typedef struct {
    npipe_notif_t n;
    bool used;
    uint32_t seq;         /* recency: higher = newer */
    uint32_t ancs_uid;
    uint8_t key_len;
    uint8_t key[NPIPE_KEY_MAX];
} npipe_slot_t;

/* Seen app. The name is kept as received (APP_SEEN carries it as text). */
typedef struct {
    bool used, dirty;
    uint8_t policy;       /* PROTO_APP_POLICY_* */
    uint8_t source;       /* PROTO_NOTIF_SOURCE_* of the last notification */
    uint8_t id_len, name_len;
    uint32_t last_seen;   /* UTC seconds, 0 = unknown */
    uint32_t seq;         /* recency for LRU eviction */
    char id[NPIPE_APP_ID_MAX + 1];
    char name[NPIPE_APP_NAME_MAX + 1];
} npipe_app_t;

typedef struct {
    cfg_store_t *cfg;                        /* filter settings; policy changes bump its revision */
    const kv_storage_t *kv;                  /* seen-apps table, one record per app slot */
    npipe_slot_t *slots;                     /* notification store */
    size_t n_slots;                          /* 1..PROTO_LIMIT_NOTIF_HISTORY_MAX */
    npipe_app_t *apps;                       /* seen apps */
    size_t n_apps;                           /* <= PROTO_LIMIT_SEEN_APPS_MAX */
    uint32_t (*now_utc)(void *ctx);          /* 0 = clock not set; NULL = no clock */
    proto_renderable_fn renderable;          /* font coverage for text normalisation; NULL = reference set */
    /* Changes to shown notifications: n->kind is added, modified or removed. */
    void (*on_event)(void *ctx, const npipe_notif_t *n);
    void (*on_app_seen)(void *ctx, const npipe_app_t *a); /* first notification from an app */
    /* An app left the seen list: APPS_FORGET, or evicted to make room for a new app (id is only valid
     * during the call). */
    void (*on_app_removed)(void *ctx, const uint8_t *id, size_t len);
    void *ctx;
} npipe_params_t;

typedef struct {
    npipe_params_t p;
    uint32_t next_id;
    uint32_t seq;
    uint32_t app_seq;
} npipe_t;

typedef struct {
    const uint8_t *p;
    size_t len;
} npipe_str_t;

/* A notification the user dismissed on the board (protocol.md 6.5). */
typedef struct {
    uint32_t id;
    uint8_t source;       /* PROTO_NOTIF_SOURCE_* */
    uint32_t ancs_uid;    /* ANCS: the UID for the negative action (M5) */
    uint8_t flags;        /* the notification's PROTO_NOTIF_FLAGS_* (ANCS: NEGATIVE_ACTION means iOS accepts the action) */
    uint8_t key_len;      /* Android: the NOTIF_POST key */
    uint8_t key[NPIPE_KEY_MAX];
} npipe_dismissed_t;

/* Load the seen apps from storage; the notification store starts empty. */
void npipe_init(npipe_t *np, const npipe_params_t *p);

/* ---------------------------------------------------------------- Android (NOTIF_*) */

typedef struct {
    npipe_str_t key, app_id, app_name, title, subtitle, body;
    uint32_t timestamp;   /* 0 = now */
    uint8_t category, priority, flags;
} npipe_android_t;

/* key and app_id must be 1-64 bytes of valid UTF-8 (guaranteed by message validation). */
proto_status_t npipe_android_post(npipe_t *np, const npipe_android_t *in, uint32_t *id, bool *shown);
proto_status_t npipe_android_remove(npipe_t *np, const uint8_t *key, size_t len); /* OK or NOT_FOUND */
void npipe_android_clear(npipe_t *np);
/* The user dismissed notification `id` on the board: removed like a removal from its source (a
 * `removed` event if it was shown). OK, or NOT_FOUND for an id the board does not hold. */
proto_status_t npipe_dismiss(npipe_t *np, uint32_t id, npipe_dismissed_t *out);

/* ---------------------------------------------------------------- ANCS */

typedef struct {
    proto_ancs_ns_t ns;
    npipe_str_t app_id, app_name, title, subtitle, message, date; /* absent: len 0 */
} npipe_ancs_t;

/* Fill attributes from a completed Data Source response (app_name comes from the app-attributes
 * cache of the ANCS session). */
void npipe_ancs_from_ds(npipe_ancs_t *in, const proto_ancs_ns_t *ns, const proto_ancs_ds_t *ds,
                        npipe_str_t app_name);
/* Added, modified or removed (ns.event_id). Removal needs no attributes. */
void npipe_ancs_event(npipe_t *np, const npipe_ancs_t *in);
/* New ANCS session: every ANCS notification is dropped (iOS re-sends pre-existing ones). */
void npipe_ancs_session_reset(npipe_t *np);

/* ---------------------------------------------------------------- store access */

size_t npipe_count(const npipe_t *np, bool shown_only);
const npipe_notif_t *npipe_get(const npipe_t *np, uint32_t id);
/* Shown notifications, newest first. Returns how many were written to out. */
size_t npipe_list(const npipe_t *np, const npipe_notif_t **out, size_t cap);

/* ---------------------------------------------------------------- seen apps */

const npipe_app_t *npipe_app_find(const npipe_t *np, const uint8_t *id, size_t len);
size_t npipe_app_count(const npipe_t *np);
/* Adds the app if needed. NO_SPACE if the list is full of apps with explicit policies; INTERNAL on
 * storage failure. *changed is set when the list or a policy changed (the caller bumps the revision). */
proto_status_t npipe_app_set_policy(npipe_t *np, const uint8_t *id, size_t len, uint8_t policy, bool *changed);
/* Remove one app (id != NULL) or all. Returns true if anything was removed. */
bool npipe_app_forget(npipe_t *np, const uint8_t *id, size_t len);
/* Persist last-seen/name updates (kept in RAM to spare flash; call e.g. every few minutes). */
void npipe_apps_flush(npipe_t *np);

/* ---------------------------------------------------------------- messages */

/* NOTIF_POST/REMOVE/CLEAR and APPS_LIST/SET_POLICY/FORGET (already validated). Writes the complete
 * response into w (cap >= the channel's max_message). Returns false for other types. */
bool npipe_handle(npipe_t *np, const proto_msg_t *req, proto_writer_t *w);
void npipe_write_app_seen_event(proto_writer_t *w, const npipe_app_t *a);
void npipe_write_dismissed_event(proto_writer_t *w, const npipe_dismissed_t *d);

#ifdef __cplusplus
}
#endif
