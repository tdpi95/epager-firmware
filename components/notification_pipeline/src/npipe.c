#include "epager/npipe.h"

#include <string.h>

/* Seen-app record, one kv key per slot ("app000".."app127"):
 *   'A' | version 1 | policy | source | seq u32 | last_seen u32 | id_len u8 | id | name_len u8 | name */
#define APP_REC_VERSION 1u
#define APP_REC_FIXED 14u
#define APP_REC_MAX (APP_REC_FIXED + NPIPE_APP_ID_MAX + NPIPE_APP_NAME_MAX)
#define NOTIF_FLAGS_KNOWN 0x1Fu
#define UNKNOWN_APP_ID "unknown"

static uint32_t now(const npipe_t *np) { return np->p.now_utc ? np->p.now_utc(np->p.ctx) : 0; }

/* Longest prefix of s within max bytes that ends on a code point boundary. */
static size_t utf8_prefix(const uint8_t *s, size_t len, size_t max)
{
    size_t pos = 0;
    while (pos < len) {
        uint32_t cp;
        size_t n = proto_utf8_decode(s + pos, len - pos, &cp);
        if (pos + n > max) {
            break;
        }
        pos += n;
    }
    return pos;
}

/* ---------------------------------------------------------------- seen apps */

static void app_key(size_t i, char out[8])
{
    out[0] = 'a';
    out[1] = 'p';
    out[2] = 'p';
    out[3] = (char)('0' + i / 100 % 10);
    out[4] = (char)('0' + i / 10 % 10);
    out[5] = (char)('0' + i % 10);
    out[6] = '\0';
}

static bool app_save(npipe_t *np, size_t i)
{
    const npipe_app_t *a = &np->p.apps[i];
    uint8_t b[APP_REC_MAX] = {'A', APP_REC_VERSION, a->policy, a->source};
    proto_wr32(b + 4, a->seq);
    proto_wr32(b + 8, a->last_seen);
    b[12] = a->id_len;
    memcpy(b + 13, a->id, a->id_len);
    size_t n = 13u + a->id_len;
    b[n++] = a->name_len;
    memcpy(b + n, a->name, a->name_len);
    n += a->name_len;
    char key[8];
    app_key(i, key);
    bool ok = np->p.kv->set(np->p.kv->ctx, key, b, n) == KV_OK;
    np->p.apps[i].dirty = !ok;
    return ok;
}

static bool app_load(npipe_app_t *a, const uint8_t *b, size_t n)
{
    if (n < APP_REC_FIXED || b[0] != 'A' || b[1] != APP_REC_VERSION) {
        return false;
    }
    size_t id_len = b[12];
    if (id_len == 0 || id_len > NPIPE_APP_ID_MAX || 13 + id_len >= n) {
        return false;
    }
    size_t name_len = b[13 + id_len];
    if (name_len > NPIPE_APP_NAME_MAX || APP_REC_FIXED + id_len + name_len != n || !proto_utf8_valid(b + 13, id_len)) {
        return false;
    }
    memset(a, 0, sizeof(*a));
    a->used = true;
    a->policy = b[2] <= PROTO_APP_POLICY_BLOCK ? b[2] : PROTO_APP_POLICY_FALLBACK;
    a->source = b[3] <= PROTO_NOTIF_SOURCE_ANDROID ? b[3] : PROTO_NOTIF_SOURCE_FALLBACK;
    a->seq = proto_rd32(b + 4);
    a->last_seen = proto_rd32(b + 8);
    a->id_len = (uint8_t)id_len;
    memcpy(a->id, b + 13, id_len);
    a->name_len = (uint8_t)name_len;
    memcpy(a->name, b + 14 + id_len, name_len);
    return true;
}

static npipe_app_t *app_lookup(const npipe_t *np, const uint8_t *id, size_t len)
{
    for (size_t i = 0; i < np->p.n_apps; i++) {
        npipe_app_t *a = &np->p.apps[i];
        if (a->used && a->id_len == len && memcmp(a->id, id, len) == 0) {
            return a;
        }
    }
    return NULL;
}

/* A free slot, else the least recently seen app without an explicit policy; -1 if none. */
static int app_alloc(const npipe_t *np)
{
    int lru = -1;
    for (size_t i = 0; i < np->p.n_apps; i++) {
        const npipe_app_t *a = &np->p.apps[i];
        if (!a->used) {
            return (int)i;
        }
        if (a->policy == PROTO_APP_POLICY_DEFAULT && (lru < 0 || a->seq < np->p.apps[lru].seq)) {
            lru = (int)i;
        }
    }
    return lru;
}

static int app_new(npipe_t *np, const uint8_t *id, size_t len)
{
    int i = app_alloc(np);
    if (i >= 0) {
        npipe_app_t *a = &np->p.apps[i];
        if (a->used && np->p.on_app_removed) {
            np->p.on_app_removed(np->p.ctx, (const uint8_t *)a->id, a->id_len); /* evicted */
        }
        memset(a, 0, sizeof(*a));
        a->used = true;
        a->id_len = (uint8_t)len;
        memcpy(a->id, id, len);
        a->seq = ++np->app_seq;
    }
    return i;
}

/* Record a notification from an app; returns NULL if the app cannot be tracked (list full of
 * explicit policies), in which case the default policy applies. */
static npipe_app_t *app_touch(npipe_t *np, const char *id, size_t len, const npipe_str_t *name, uint8_t source)
{
    size_t name_len = name->len ? utf8_prefix(name->p, name->len, NPIPE_APP_NAME_MAX) : 0;
    npipe_app_t *a = app_lookup(np, (const uint8_t *)id, len);
    bool fresh = !a;
    if (fresh) {
        int i = app_new(np, (const uint8_t *)id, len);
        if (i < 0) {
            return NULL;
        }
        a = &np->p.apps[i];
    }
    a->seq = ++np->app_seq;
    a->last_seen = now(np);
    a->source = source;
    if (name_len) {
        a->name_len = (uint8_t)name_len;
        memcpy(a->name, name->p, name_len);
        a->name[name_len] = '\0';
    }
    a->dirty = true; /* recency is persisted lazily (npipe_apps_flush) */
    if (fresh) {
        (void)app_save(np, (size_t)(a - np->p.apps));
        if (np->p.on_app_seen) {
            np->p.on_app_seen(np->p.ctx, a);
        }
    }
    return a;
}

const npipe_app_t *npipe_app_find(const npipe_t *np, const uint8_t *id, size_t len) { return app_lookup(np, id, len); }

size_t npipe_app_count(const npipe_t *np)
{
    size_t n = 0;
    for (size_t i = 0; i < np->p.n_apps; i++) {
        n += np->p.apps[i].used;
    }
    return n;
}

proto_status_t npipe_app_set_policy(npipe_t *np, const uint8_t *id, size_t len, uint8_t policy, bool *changed)
{
    if (policy > PROTO_APP_POLICY_BLOCK) {
        policy = PROTO_APP_POLICY_FALLBACK;
    }
    *changed = false;
    npipe_app_t *a = app_lookup(np, id, len);
    if (a && a->policy == policy) {
        return PROTO_OK;
    }
    if (!a) {
        int i = app_new(np, id, len);
        if (i < 0) {
            return PROTO_NO_SPACE;
        }
        a = &np->p.apps[i];
    }
    a->policy = policy;
    *changed = true;
    return app_save(np, (size_t)(a - np->p.apps)) ? PROTO_OK : PROTO_INTERNAL;
}

bool npipe_app_forget(npipe_t *np, const uint8_t *id, size_t len)
{
    bool removed = false;
    for (size_t i = 0; i < np->p.n_apps; i++) {
        npipe_app_t *a = &np->p.apps[i];
        if (a->used && (!id || (a->id_len == len && memcmp(a->id, id, len) == 0))) {
            char key[8];
            app_key(i, key);
            (void)np->p.kv->erase(np->p.kv->ctx, key); /* a stale record is overwritten on reuse */
            a->used = false;
            removed = true;
            if (np->p.on_app_removed) {
                np->p.on_app_removed(np->p.ctx, (const uint8_t *)a->id, a->id_len);
            }
        }
    }
    return removed;
}

void npipe_apps_flush(npipe_t *np)
{
    for (size_t i = 0; i < np->p.n_apps; i++) {
        if (np->p.apps[i].used && np->p.apps[i].dirty) {
            (void)app_save(np, i);
        }
    }
}

void npipe_init(npipe_t *np, const npipe_params_t *p)
{
    memset(np, 0, sizeof(*np));
    np->p = *p;
    np->next_id = 1;
    memset(p->slots, 0, p->n_slots * sizeof(p->slots[0]));
    for (size_t i = 0; i < p->n_apps; i++) {
        uint8_t b[APP_REC_MAX];
        size_t n = 0;
        char key[8];
        app_key(i, key);
        npipe_app_t loaded;
        p->apps[i].used = false;
        kv_status_t st = p->kv->get(p->kv->ctx, key, b, sizeof(b), &n);
        if (st == KV_OK && app_load(&loaded, b, n) && !app_lookup(np, (const uint8_t *)loaded.id, loaded.id_len)) {
            p->apps[i] = loaded;
            if (loaded.seq > np->app_seq) {
                np->app_seq = loaded.seq;
            }
        } else if (st != KV_NOT_FOUND) {
            (void)p->kv->erase(p->kv->ctx, key); /* corrupt or duplicate record */
        }
    }
}

/* ---------------------------------------------------------------- filters (protocol.md 6.3) */

static bool in_quiet_hours(const npipe_t *np) { return cfg_quiet_hours(np->p.cfg, now(np), NULL); }

static bool passes(const npipe_t *np, const npipe_app_t *app, const npipe_notif_t *n)
{
    const cfg_store_t *c = np->p.cfg;
    uint8_t policy = app ? app->policy : PROTO_APP_POLICY_DEFAULT;
    if (policy == PROTO_APP_POLICY_BLOCK ||
        (policy == PROTO_APP_POLICY_DEFAULT &&
         cfg_get_int(c, PROTO_CFG_NOTIF_DEFAULT_APP_POLICY) == PROTO_DEFAULT_POLICY_BLOCK)) {
        return false;
    }
    if (!((uint32_t)cfg_get_int(c, PROTO_CFG_NOTIF_CATEGORY_MASK) & (1u << n->category)) ||
        n->priority < cfg_get_int(c, PROTO_CFG_NOTIF_MIN_PRIORITY)) {
        return false;
    }
    if ((n->flags & PROTO_NOTIF_FLAGS_SILENT) && !cfg_get_int(c, PROTO_CFG_NOTIF_SHOW_SILENT)) {
        return false;
    }
    return !in_quiet_hours(np);
}

/* ---------------------------------------------------------------- store */

typedef struct {
    uint8_t source, category, priority, flags;
    uint32_t timestamp;
    const char *app_id; /* valid UTF-8, 1..64 bytes */
    size_t app_id_len;
    npipe_str_t app_name, title, subtitle, body;
} input_t;

static void emit(npipe_t *np, npipe_slot_t *s, npipe_kind_t kind)
{
    s->n.kind = (uint8_t)kind;
    if (np->p.on_event) {
        np->p.on_event(np->p.ctx, &s->n);
    }
}

static void remove_slot(npipe_t *np, npipe_slot_t *s)
{
    if (s->n.shown) {
        emit(np, s, NPIPE_REMOVED);
    }
    s->used = false;
}

/* A free slot, else evict the oldest hidden notification, else the oldest one. */
static npipe_slot_t *alloc_slot(npipe_t *np)
{
    npipe_slot_t *victim = NULL;
    for (size_t i = 0; i < np->p.n_slots; i++) {
        npipe_slot_t *s = &np->p.slots[i];
        if (!s->used) {
            victim = s;
            break;
        }
        if (!victim || (victim->n.shown && !s->n.shown) || (victim->n.shown == s->n.shown && s->seq < victim->seq)) {
            victim = s;
        }
    }
    if (victim->used) {
        remove_slot(np, victim);
    }
    memset(victim, 0, sizeof(*victim));
    victim->used = true;
    victim->n.id = np->next_id++;
    if (np->next_id == 0) {
        np->next_id = 1;
    }
    return victim;
}

static uint16_t norm(const npipe_t *np, const npipe_str_t *s, char *dst, size_t cap)
{
    size_t n = s->len ? proto_text_normalize(s->p, s->len, (uint8_t *)dst, cap, PROTO_TEXT_NO_BUDGET,
                                             np->p.renderable, np->p.ctx)
                      : 0;
    dst[n] = '\0';
    return (uint16_t)n;
}

static void ingest(npipe_t *np, npipe_slot_t *s, bool existed, const input_t *in)
{
    npipe_notif_t *n = &s->n;
    bool was_shown = existed && n->shown;
    n->source = in->source;
    n->category = in->category <= PROTO_CATEGORY_ENTERTAINMENT ? in->category : PROTO_CATEGORY_FALLBACK;
    n->priority = in->priority <= PROTO_PRIORITY_URGENT ? in->priority : PROTO_PRIORITY_FALLBACK;
    n->flags = in->flags & NOTIF_FLAGS_KNOWN;
    n->timestamp = in->timestamp ? in->timestamp : now(np);
    n->app_id_len = (uint16_t)in->app_id_len;
    memcpy(n->app_id, in->app_id, in->app_id_len);
    n->app_id[in->app_id_len] = '\0';
    n->app_name_len = norm(np, &in->app_name, n->app_name, NPIPE_APP_NAME_MAX);
    n->title_len = norm(np, &in->title, n->title, NPIPE_TITLE_MAX);
    n->subtitle_len = norm(np, &in->subtitle, n->subtitle, NPIPE_SUBTITLE_MAX);
    n->body_len = norm(np, &in->body, n->body, NPIPE_BODY_MAX);
    const npipe_app_t *app = app_touch(np, in->app_id, in->app_id_len, &in->app_name, in->source);
    n->shown = passes(np, app, n);
    s->seq = ++np->seq;
    if (was_shown) {
        emit(np, s, n->shown ? NPIPE_MODIFIED : NPIPE_REMOVED);
    } else if (n->shown) {
        emit(np, s, NPIPE_ADDED);
    } else {
        n->kind = existed ? NPIPE_MODIFIED : NPIPE_ADDED;
    }
}

static npipe_slot_t *find_android(const npipe_t *np, const uint8_t *key, size_t len)
{
    for (size_t i = 0; i < np->p.n_slots; i++) {
        npipe_slot_t *s = &np->p.slots[i];
        if (s->used && s->n.source == PROTO_NOTIF_SOURCE_ANDROID && s->key_len == len && memcmp(s->key, key, len) == 0) {
            return s;
        }
    }
    return NULL;
}

static npipe_slot_t *find_ancs(const npipe_t *np, uint32_t uid)
{
    for (size_t i = 0; i < np->p.n_slots; i++) {
        npipe_slot_t *s = &np->p.slots[i];
        if (s->used && s->n.source == PROTO_NOTIF_SOURCE_ANCS && s->ancs_uid == uid) {
            return s;
        }
    }
    return NULL;
}

static void remove_source(npipe_t *np, uint8_t source)
{
    for (size_t i = 0; i < np->p.n_slots; i++) {
        if (np->p.slots[i].used && np->p.slots[i].n.source == source) {
            remove_slot(np, &np->p.slots[i]);
        }
    }
}

proto_status_t npipe_android_post(npipe_t *np, const npipe_android_t *a, uint32_t *id, bool *shown)
{
    if (a->key.len == 0 || a->key.len > NPIPE_KEY_MAX || a->app_id.len == 0 || a->app_id.len > NPIPE_APP_ID_MAX ||
        !proto_utf8_valid(a->app_id.p, a->app_id.len)) {
        return PROTO_MALFORMED; /* message validation guarantees these */
    }
    npipe_slot_t *s = find_android(np, a->key.p, a->key.len);
    bool existed = s != NULL;
    if (!s) {
        s = alloc_slot(np);
        s->key_len = (uint8_t)a->key.len;
        memcpy(s->key, a->key.p, a->key.len);
    }
    input_t in = {PROTO_NOTIF_SOURCE_ANDROID, a->category, a->priority, a->flags, a->timestamp,
                  (const char *)a->app_id.p, a->app_id.len, a->app_name, a->title, a->subtitle, a->body};
    ingest(np, s, existed, &in);
    *id = s->n.id;
    *shown = s->n.shown;
    return PROTO_OK;
}

proto_status_t npipe_android_remove(npipe_t *np, const uint8_t *key, size_t len)
{
    npipe_slot_t *s = find_android(np, key, len);
    if (!s) {
        return PROTO_NOT_FOUND;
    }
    remove_slot(np, s);
    return PROTO_OK;
}

void npipe_android_clear(npipe_t *np) { remove_source(np, PROTO_NOTIF_SOURCE_ANDROID); }

proto_status_t npipe_dismiss(npipe_t *np, uint32_t id, npipe_dismissed_t *out)
{
    for (size_t i = 0; i < np->p.n_slots; i++) {
        npipe_slot_t *s = &np->p.slots[i];
        if (s->used && s->n.id == id) {
            *out = (npipe_dismissed_t){.id = id, .source = s->n.source, .ancs_uid = s->ancs_uid, .flags = s->n.flags,
                                       .key_len = s->n.source == PROTO_NOTIF_SOURCE_ANDROID ? s->key_len : 0};
            memcpy(out->key, s->key, out->key_len);
            remove_slot(np, s);
            return PROTO_OK;
        }
    }
    return PROTO_NOT_FOUND;
}

/* ---------------------------------------------------------------- ANCS */

static void ds_attr(const proto_ancs_ds_t *ds, uint8_t id, npipe_str_t *out)
{
    const uint8_t *v;
    uint16_t len;
    if (proto_ancs_ds_attr(ds, id, &v, &len)) {
        out->p = v;
        out->len = len;
    }
}

void npipe_ancs_from_ds(npipe_ancs_t *in, const proto_ancs_ns_t *ns, const proto_ancs_ds_t *ds, npipe_str_t app_name)
{
    memset(in, 0, sizeof(*in));
    in->ns = *ns;
    in->app_name = app_name;
    ds_attr(ds, PROTO_ANCS_ATTR_APP_ID, &in->app_id);
    ds_attr(ds, PROTO_ANCS_ATTR_TITLE, &in->title);
    ds_attr(ds, PROTO_ANCS_ATTR_SUBTITLE, &in->subtitle);
    ds_attr(ds, PROTO_ANCS_ATTR_MESSAGE, &in->message);
    ds_attr(ds, PROTO_ANCS_ATTR_DATE, &in->date);
}

static uint32_t ancs_timestamp(const npipe_t *np, const npipe_str_t *date)
{
    int64_t local;
    if (date->len && proto_ancs_parse_date(date->p, date->len, &local) == PROTO_OK) {
        int64_t utc = local - (int64_t)cfg_get_int(np->p.cfg, PROTO_CFG_TIME_TZ_OFFSET_MIN) * 60;
        if (utc > 0 && utc <= (int64_t)UINT32_MAX) {
            return (uint32_t)utc;
        }
    }
    return 0; /* ingest() uses the current time */
}

void npipe_ancs_event(npipe_t *np, const npipe_ancs_t *in)
{
    const proto_ancs_ns_t *ns = &in->ns;
    npipe_slot_t *s = find_ancs(np, ns->uid);
    if (ns->event_id == PROTO_ANCS_EVENT_REMOVED) {
        if (s) {
            remove_slot(np, s);
        }
        return;
    }
    if (ns->event_id != PROTO_ANCS_EVENT_ADDED && ns->event_id != PROTO_ANCS_EVENT_MODIFIED) {
        return; /* reserved event IDs */
    }
    uint8_t prio = PROTO_PRIORITY_NORMAL;
    if (ns->category_id == PROTO_CATEGORY_INCOMING_CALL) {
        prio = PROTO_PRIORITY_URGENT;
    } else if (ns->event_flags & PROTO_ANCS_FLAG_IMPORTANT) {
        prio = PROTO_PRIORITY_HIGH;
    } else if (ns->event_flags & PROTO_ANCS_FLAG_SILENT) {
        prio = PROTO_PRIORITY_LOW;
    }
    /* The app ID keys the seen-apps table and is sent as a strict UTF-8 `str`. */
    const char *app_id = UNKNOWN_APP_ID;
    size_t app_id_len = sizeof(UNKNOWN_APP_ID) - 1;
    size_t cut = utf8_prefix(in->app_id.p, in->app_id.len, NPIPE_APP_ID_MAX);
    if (cut && proto_utf8_valid(in->app_id.p, cut)) {
        app_id = (const char *)in->app_id.p;
        app_id_len = cut;
    }
    bool existed = s != NULL;
    if (!s) {
        s = alloc_slot(np);
        s->ancs_uid = ns->uid;
    }
    input_t x = {PROTO_NOTIF_SOURCE_ANCS, ns->category_id, prio, ns->event_flags, ancs_timestamp(np, &in->date),
                 app_id, app_id_len, in->app_name, in->title, in->subtitle, in->message};
    ingest(np, s, existed, &x);
}

void npipe_ancs_session_reset(npipe_t *np) { remove_source(np, PROTO_NOTIF_SOURCE_ANCS); }

/* ---------------------------------------------------------------- store access */

size_t npipe_count(const npipe_t *np, bool shown_only)
{
    size_t n = 0;
    for (size_t i = 0; i < np->p.n_slots; i++) {
        n += np->p.slots[i].used && (!shown_only || np->p.slots[i].n.shown);
    }
    return n;
}

const npipe_notif_t *npipe_get(const npipe_t *np, uint32_t id)
{
    for (size_t i = 0; i < np->p.n_slots; i++) {
        if (np->p.slots[i].used && np->p.slots[i].n.id == id) {
            return &np->p.slots[i].n;
        }
    }
    return NULL;
}

size_t npipe_list(const npipe_t *np, const npipe_notif_t **out, size_t cap)
{
    size_t n = 0;
    uint32_t below = UINT32_MAX; /* seq values are unique: repeatedly take the newest older one */
    while (n < cap) {
        const npipe_slot_t *best = NULL;
        for (size_t i = 0; i < np->p.n_slots; i++) {
            const npipe_slot_t *s = &np->p.slots[i];
            if (s->used && s->n.shown && s->seq < below && (!best || s->seq > best->seq)) {
                best = s;
            }
        }
        if (!best) {
            break;
        }
        out[n++] = &best->n;
        below = best->seq;
    }
    return n;
}

/* ---------------------------------------------------------------- messages */

static void write_app(proto_writer_t *w, uint8_t tag, const npipe_app_t *a)
{
    proto_w_struct_begin(w, tag);
    proto_w_bytes(w, PROTO_TAG_APP_ENTRY_APP_ID, a->id, a->id_len);
    if (a->name_len) {
        proto_w_bytes(w, PROTO_TAG_APP_ENTRY_NAME, a->name, a->name_len);
    }
    proto_w_u8(w, PROTO_TAG_APP_ENTRY_POLICY, a->policy);
    proto_w_u8(w, PROTO_TAG_APP_ENTRY_SOURCE, a->source);
    if (a->last_seen) {
        proto_w_u32(w, PROTO_TAG_APP_ENTRY_LAST_SEEN, a->last_seen);
    }
    proto_w_struct_end(w);
}

static size_t app_entry_size(const npipe_app_t *a)
{
    return PROTO_TLV_HDR_LEN + (PROTO_TLV_HDR_LEN + a->id_len) + (a->name_len ? PROTO_TLV_HDR_LEN + a->name_len : 0) +
           2 * (PROTO_TLV_HDR_LEN + 1) + (a->last_seen ? PROTO_TLV_HDR_LEN + 4 : 0);
}

void npipe_write_app_seen_event(proto_writer_t *w, const npipe_app_t *a)
{
    proto_w_header(w, PROTO_MSG_APP_SEEN, 0);
    write_app(w, PROTO_TAG_APP_SEEN_APP, a);
}

void npipe_write_dismissed_event(proto_writer_t *w, const npipe_dismissed_t *d)
{
    proto_w_header(w, PROTO_MSG_NOTIF_DISMISSED, 0);
    proto_w_u32(w, PROTO_TAG_NOTIF_DISMISSED_NOTIF_ID, d->id);
    proto_w_u8(w, PROTO_TAG_NOTIF_DISMISSED_SOURCE, d->source);
    if (d->key_len) {
        proto_w_bytes(w, PROTO_TAG_NOTIF_DISMISSED_KEY, d->key, d->key_len);
    }
}

static npipe_str_t field(const proto_msg_t *req, uint8_t tag)
{
    npipe_str_t s = {NULL, 0};
    uint16_t len;
    if (proto_find(req->body, req->body_len, tag, &s.p, &len)) {
        s.len = len;
    }
    return s;
}

static void handle_post(npipe_t *np, const proto_msg_t *req, proto_writer_t *w)
{
    npipe_android_t a = {
        .key = field(req, PROTO_TAG_NOTIF_POST_KEY),
        .app_id = field(req, PROTO_TAG_NOTIF_POST_APP_ID),
        .app_name = field(req, PROTO_TAG_NOTIF_POST_APP_NAME),
        .title = field(req, PROTO_TAG_NOTIF_POST_TITLE),
        .subtitle = field(req, PROTO_TAG_NOTIF_POST_SUBTITLE),
        .body = field(req, PROTO_TAG_NOTIF_POST_BODY),
        .category = PROTO_CATEGORY_OTHER,
        .priority = PROTO_PRIORITY_NORMAL,
    };
    (void)proto_get_u32(req->body, req->body_len, PROTO_TAG_NOTIF_POST_TIMESTAMP, &a.timestamp);
    (void)proto_get_u8(req->body, req->body_len, PROTO_TAG_NOTIF_POST_CATEGORY, &a.category);
    (void)proto_get_u8(req->body, req->body_len, PROTO_TAG_NOTIF_POST_PRIORITY, &a.priority);
    (void)proto_get_u8(req->body, req->body_len, PROTO_TAG_NOTIF_POST_FLAGS, &a.flags);
    uint32_t id = 0;
    bool shown = false;
    proto_status_t st = npipe_android_post(np, &a, &id, &shown);
    proto_w_response(w, req, st);
    if (st == PROTO_OK) {
        proto_w_u32(w, PROTO_TAG_NOTIF_POST_RSP_NOTIF_ID, id);
        proto_w_bool(w, PROTO_TAG_NOTIF_POST_RSP_SHOWN, shown);
    }
}

static void handle_apps_list(const npipe_t *np, const proto_msg_t *req, proto_writer_t *w)
{
    uint16_t offset = 0;
    uint8_t limit = 16;
    (void)proto_get_u16(req->body, req->body_len, PROTO_TAG_APPS_LIST_OFFSET, &offset);
    (void)proto_get_u8(req->body, req->body_len, PROTO_TAG_APPS_LIST_LIMIT, &limit);
    proto_w_response(w, req, PROTO_OK);
    proto_w_u16(w, PROTO_TAG_APPS_LIST_RSP_TOTAL, (uint16_t)npipe_app_count(np));
    size_t index = 0, written = 0;
    for (size_t i = 0; i < np->p.n_apps && written < limit; i++) {
        const npipe_app_t *a = &np->p.apps[i];
        if (!a->used || index++ < offset) {
            continue;
        }
        if (w->len + app_entry_size(a) > PROTO_LIMIT_MAX_MESSAGE_CONFIG) {
            break; /* the app pages on (protocol.md 5) */
        }
        write_app(w, PROTO_TAG_APPS_LIST_RSP_APP, a);
        written++;
    }
}

/* Policy changes bump the config revision; the response carries it. */
static void respond_revision(npipe_t *np, const proto_msg_t *req, proto_writer_t *w, proto_status_t st, bool changed,
                             uint8_t rev_tag)
{
    if (changed && cfg_bump_revision(np->p.cfg) != PROTO_OK && st == PROTO_OK) {
        st = PROTO_INTERNAL;
    }
    proto_w_response(w, req, st);
    if (st == PROTO_OK) {
        proto_w_u32(w, rev_tag, cfg_revision(np->p.cfg));
    }
}

static void handle_set_policy(npipe_t *np, const proto_msg_t *req, proto_writer_t *w)
{
    size_t pos = 0;
    const uint8_t *v;
    uint16_t vl;
    bool changed = false;
    proto_status_t st = PROTO_OK;
    while (st == PROTO_OK && proto_find_next(req->body, req->body_len, PROTO_TAG_APPS_SET_POLICY_ENTRY, &pos, &v, &vl)) {
        const uint8_t *id;
        uint16_t id_len;
        uint8_t policy = PROTO_APP_POLICY_DEFAULT;
        bool c = false;
        (void)proto_find(v, vl, PROTO_TAG_POLICY_ENTRY_APP_ID, &id, &id_len); /* required: validated */
        (void)proto_get_u8(v, vl, PROTO_TAG_POLICY_ENTRY_POLICY, &policy);
        st = npipe_app_set_policy(np, id, id_len, policy, &c);
        changed |= c;
    }
    respond_revision(np, req, w, st, changed, PROTO_TAG_APPS_SET_POLICY_RSP_REVISION);
}

static void handle_forget(npipe_t *np, const proto_msg_t *req, proto_writer_t *w)
{
    size_t pos = 0;
    const uint8_t *v;
    uint16_t vl;
    bool changed = false, any = false;
    while (proto_find_next(req->body, req->body_len, PROTO_TAG_APPS_FORGET_APP_ID, &pos, &v, &vl)) {
        any = true;
        changed |= npipe_app_forget(np, v, vl);
    }
    if (!any) {
        changed = npipe_app_forget(np, NULL, 0);
    }
    respond_revision(np, req, w, PROTO_OK, changed, PROTO_TAG_APPS_FORGET_RSP_REVISION);
}

bool npipe_handle(npipe_t *np, const proto_msg_t *req, proto_writer_t *w)
{
    npipe_str_t key;
    switch (req->type) {
    case PROTO_MSG_NOTIF_POST:
        handle_post(np, req, w);
        return true;
    case PROTO_MSG_NOTIF_REMOVE:
        key = field(req, PROTO_TAG_NOTIF_REMOVE_KEY);
        proto_w_response(w, req, npipe_android_remove(np, key.p, key.len));
        return true;
    case PROTO_MSG_NOTIF_CLEAR:
        npipe_android_clear(np);
        proto_w_response(w, req, PROTO_OK);
        return true;
    case PROTO_MSG_APPS_LIST:
        handle_apps_list(np, req, w);
        return true;
    case PROTO_MSG_APPS_SET_POLICY:
        handle_set_policy(np, req, w);
        return true;
    case PROTO_MSG_APPS_FORGET:
        handle_forget(np, req, w);
        return true;
    default:
        return false;
    }
}
