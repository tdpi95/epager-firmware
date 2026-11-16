#include "epager/xfer.h"

#include <string.h>

/* Journal record (internal): 'J' | version 1 | kind | asset_type | size u32 | crc32 u32 |
 * sha256[32] | offset u32 | prefix_crc u32 | id_len u8 | ver_len u8 | id | version */
#define JOURNAL_VERSION 1u
#define JOURNAL_FIXED 54u
#define JOURNAL_MAX (JOURNAL_FIXED + XFER_ID_MAX + XFER_VERSION_MAX)
#define VERIFY_BLOCK 256u
#define LIST_DEFAULT_LIMIT 16u
#define FW_ID "firmware"

typedef struct {
    xfer_target_t t;
    uint32_t offset, prefix_crc;
} journal_t;

/* ---------------------------------------------------------------- names: "<type>_<id>[~|@]" */

static void file_name(uint8_t type, const char *id, size_t id_len, char suffix, char out[BLOB_NAME_MAX + 1])
{
    size_t n = 0;
    if (type >= 100) {
        out[n++] = (char)('0' + type / 100);
    }
    if (type >= 10) {
        out[n++] = (char)('0' + type / 10 % 10);
    }
    out[n++] = (char)('0' + type % 10);
    out[n++] = '_';
    memcpy(out + n, id, id_len);
    n += id_len;
    if (suffix) {
        out[n++] = suffix;
    }
    out[n] = '\0';
}

void xfer_asset_blob_name(uint8_t asset_type, const char *id, size_t id_len, char out[BLOB_NAME_MAX + 1])
{
    file_name(asset_type, id, id_len, 0, out);
}

static void target_name(const xfer_target_t *t, char suffix, char out[BLOB_NAME_MAX + 1])
{
    file_name(t->asset_type, t->id, t->id_len, suffix, out);
}

/* Installed asset names only (not temporary or version files). */
static bool parse_asset_name(const char *name, uint8_t *type, const char **id, size_t *id_len)
{
    size_t n = strlen(name), i = 0;
    unsigned v = 0;
    if (n == 0 || name[n - 1] == '~' || name[n - 1] == '@') {
        return false;
    }
    while (i < n && i < 3 && name[i] >= '0' && name[i] <= '9') {
        v = v * 10 + (unsigned)(name[i++] - '0');
    }
    if (i == 0 || i >= n || name[i] != '_' || v > 255 || n - i - 1 == 0 || n - i - 1 > XFER_ID_MAX) {
        return false;
    }
    *type = (uint8_t)v;
    *id = name + i + 1;
    *id_len = n - i - 1;
    return true;
}

/* ---------------------------------------------------------------- asset index (PLAN D128) */

/* On LittleFS every stat() or open() walks the directory's metadata log, and the app icons (88 bytes) and version files
 * live inline in it: listing the assets by asking the files cost seconds (the task watchdog fired on the Paper). The
 * index answers ASSET_LIST, STORAGE_INFO and the budget checks from RAM. Every change marks it dirty in kv first and
 * saves it after, so a change cut off by a power loss makes the next boot rebuild it from the files.
 *
 * File: 'X' 'I' | format 1 | count u16 | entries | crc32 u32 of everything before it.
 * Entry: name_len u8 | name | size u32 | ver_len u8 | ver | has_origin u8 | origin u8. Names are ascending. */
#define INDEX_FORMAT 1u
#define INDEX_HEAD 5u
#define INDEX_TEMP XFER_INDEX_FILE "~"
#define INDEX_IO 256u

/* Where `name` is in the index (*found) or would go. */
static uint16_t index_find(const xfer_t *x, const char *name, bool *found)
{
    uint16_t lo = 0, hi = x->n_index;
    while (lo < hi) {
        const uint16_t mid = (uint16_t)((lo + hi) / 2u);
        const int c = strcmp(x->index[mid].name, name);
        if (c == 0) {
            *found = true;
            return mid;
        }
        if (c < 0) {
            lo = (uint16_t)(mid + 1u);
        } else {
            hi = mid;
        }
    }
    *found = false;
    return lo;
}

static xfer_index_entry_t *index_get(xfer_t *x, const char *name)
{
    bool found;
    const uint16_t at = index_find(x, name, &found);
    return found ? &x->index[at] : NULL;
}

static bool index_has(const xfer_t *x, const char *name)
{
    bool found;
    (void)index_find(x, name, &found);
    return found;
}

/* The entry of `name` (an asset file name), new or reset, without a version; NULL when the index is full. */
static xfer_index_entry_t *index_put(xfer_t *x, const char *name, uint32_t size)
{
    bool found;
    const uint16_t at = index_find(x, name, &found);
    if (!found) {
        if (x->n_index >= XFER_INDEX_MAX) {
            return NULL;
        }
        memmove(&x->index[at + 1u], &x->index[at], (size_t)(x->n_index - at) * sizeof(x->index[0]));
        x->n_index++;
    }
    xfer_index_entry_t *e = &x->index[at];
    memset(e, 0, sizeof(*e));
    memcpy(e->name, name, strlen(name) + 1u); /* <= BLOB_NAME_MAX: an asset name */
    e->size = size;
    return e;
}

static void index_drop(xfer_t *x, const char *name)
{
    bool found;
    const uint16_t at = index_find(x, name, &found);
    if (found) {
        x->n_index--;
        memmove(&x->index[at], &x->index[at + 1u], (size_t)(x->n_index - at) * sizeof(x->index[0]));
    }
}

/* What a version file holds: version [NUL origin] (install_asset). Unusable content gives no version. */
static void index_set_meta(xfer_index_entry_t *e, const uint8_t *m, uint32_t n)
{
    uint32_t ver_len = n;
    e->has_origin = false;
    e->origin = 0;
    for (uint32_t k = 0; k < n; k++) {
        if (m[k] == 0) {
            e->has_origin = k + 2u == n;
            e->origin = e->has_origin ? m[k + 1u] : 0;
            ver_len = k;
            break;
        }
    }
    e->ver_len = ver_len <= XFER_VERSION_MAX ? (uint8_t)ver_len : 0;
    if (e->ver_len) {
        memcpy(e->ver, m, e->ver_len);
    }
}

/* Before the first change of asset or version files: a power loss from here on leaves the saved index stale. */
static void index_mark_dirty(xfer_t *x)
{
    if (x->p.index_store && !x->index_dirty) {
        const uint8_t one = 1;
        (void)x->p.kv->set(x->p.kv->ctx, XFER_INDEX_DIRTY_KEY, &one, 1);
        x->index_dirty = true;
    }
}

typedef struct {
    const blob_storage_t *s;
    uint8_t buf[INDEX_IO];
    size_t len;
    uint32_t off, crc;
    bool ok;
} index_out_t;

static void out_flush(index_out_t *o)
{
    if (o->ok && o->len) {
        o->ok = o->s->write(o->s->ctx, INDEX_TEMP, o->off, o->buf, o->len) == BLOB_OK;
        o->crc = proto_crc32_update(o->crc, o->buf, o->len);
        o->off += (uint32_t)o->len;
    }
    o->len = 0;
}

static void out_put(index_out_t *o, const void *data, size_t n) /* n <= INDEX_IO */
{
    if (o->len + n > sizeof(o->buf)) {
        out_flush(o);
    }
    memcpy(o->buf + o->len, data, n);
    o->len += n;
}

/* The index to its file (temp, then rename); a saved index is no longer dirty. */
static void index_save(xfer_t *x)
{
    const blob_storage_t *s = x->p.index_store;
    if (!s) {
        return;
    }
    index_out_t o = {.s = s, .ok = s->remove(s->ctx, INDEX_TEMP) == BLOB_OK};
    uint8_t head[INDEX_HEAD] = {'X', 'I', INDEX_FORMAT};
    proto_wr16(head + 3, x->n_index);
    out_put(&o, head, sizeof(head));
    for (uint16_t i = 0; i < x->n_index; i++) {
        const xfer_index_entry_t *e = &x->index[i];
        uint8_t v[4], name_len = (uint8_t)strlen(e->name), origin[2] = {e->has_origin, e->origin};
        out_put(&o, &name_len, 1);
        out_put(&o, e->name, name_len);
        proto_wr32(v, e->size);
        out_put(&o, v, 4);
        out_put(&o, &e->ver_len, 1);
        out_put(&o, e->ver, e->ver_len);
        out_put(&o, origin, 2);
    }
    out_flush(&o);
    uint8_t crc[4];
    proto_wr32(crc, o.crc);
    out_put(&o, crc, 4);
    out_flush(&o);
    if (o.ok && s->flush(s->ctx, INDEX_TEMP) == BLOB_OK && s->rename(s->ctx, INDEX_TEMP, XFER_INDEX_FILE) == BLOB_OK &&
        x->index_dirty && x->p.kv->erase(x->p.kv->ctx, XFER_INDEX_DIRTY_KEY) == KV_OK) {
        x->index_dirty = false;
    }
}

typedef struct {
    const blob_storage_t *s;
    uint8_t buf[INDEX_IO];
    size_t pos, have;
    uint32_t next, end, crc;
} index_in_t;

/* The next n (<= INDEX_IO) bytes of the file, NULL past its end or on a read error. */
static const uint8_t *in_take(index_in_t *in, size_t n)
{
    if (in->have - in->pos < n) {
        memmove(in->buf, in->buf + in->pos, in->have - in->pos);
        in->have -= in->pos;
        in->pos = 0;
        uint32_t want = in->end - in->next;
        if (want > sizeof(in->buf) - in->have) {
            want = (uint32_t)(sizeof(in->buf) - in->have);
        }
        if (want && in->s->read(in->s->ctx, XFER_INDEX_FILE, in->next, in->buf + in->have, want) != BLOB_OK) {
            return NULL;
        }
        in->next += want;
        in->have += want;
        if (in->have < n) {
            return NULL;
        }
    }
    const uint8_t *p = in->buf + in->pos;
    in->pos += n;
    in->crc = proto_crc32_update(in->crc, p, n);
    return p;
}

/* The saved index, if it is whole and well-formed. */
static bool index_load(xfer_t *x)
{
    const blob_storage_t *s = x->p.index_store;
    index_in_t in = {.s = s};
    x->n_index = 0;
    if (!s || s->stat(s->ctx, XFER_INDEX_FILE, &in.end) != BLOB_OK) {
        return false;
    }
    const uint8_t *p = in_take(&in, INDEX_HEAD);
    if (!p || p[0] != 'X' || p[1] != 'I' || p[2] != INDEX_FORMAT || proto_rd16(p + 3) > XFER_INDEX_MAX) {
        return false;
    }
    const uint16_t count = proto_rd16(p + 3);
    for (uint16_t i = 0; i < count; i++) {
        xfer_index_entry_t *e = &x->index[i];
        uint8_t type;
        const char *id;
        size_t id_len;
        memset(e, 0, sizeof(*e));
        if (!(p = in_take(&in, 1)) || p[0] == 0 || p[0] > BLOB_NAME_MAX) {
            return false;
        }
        const size_t name_len = p[0];
        if (!(p = in_take(&in, name_len))) {
            return false;
        }
        memcpy(e->name, p, name_len);
        if (memchr(e->name, 0, name_len) || !parse_asset_name(e->name, &type, &id, &id_len) ||
            (i > 0 && strcmp(x->index[i - 1u].name, e->name) >= 0) || !(p = in_take(&in, 4))) {
            return false;
        }
        e->size = proto_rd32(p);
        if (!(p = in_take(&in, 1)) || p[0] > XFER_VERSION_MAX) {
            return false;
        }
        e->ver_len = p[0];
        if (e->ver_len) {
            if (!(p = in_take(&in, e->ver_len))) {
                return false;
            }
            memcpy(e->ver, p, e->ver_len);
        }
        if (!(p = in_take(&in, 2)) || p[0] > 1) {
            return false;
        }
        e->has_origin = p[0] == 1;
        e->origin = p[1];
    }
    const uint32_t crc = in.crc;
    if (!(p = in_take(&in, 4)) || proto_rd32(p) != crc || in.next != in.end || in.pos != in.have) {
        return false;
    }
    x->n_index = count;
    return true;
}

/* The index from the files themselves: a stat per asset and a read of its version file, each a walk of the directory
 * on LittleFS, so this runs only when the saved index cannot be trusted. Afterwards no version file is without its
 * asset, and no asset is left out of the index (past XFER_INDEX_MAX, which the local limits keep away, it is deleted). */
static void index_rebuild(xfer_t *x)
{
    const blob_storage_t *b = x->p.blob;
    char name[BLOB_NAME_MAX + 1], meta[BLOB_NAME_MAX + 1];
    uint32_t i = 0, size;
    bool complete = true;
    x->n_index = 0;
    while (b->list(b->ctx, i++, name, sizeof(name), NULL) == BLOB_OK) {
        uint8_t type;
        const char *id;
        size_t id_len;
        if (parse_asset_name(name, &type, &id, &id_len)) {
            if (b->stat(b->ctx, name, &size) == BLOB_OK) {
                (void)index_put(x, name, size); /* NULL when full: deleted below */
            } else {
                complete = false;
            }
        }
    }
    for (uint16_t k = 0; k < x->n_index; k++) {
        xfer_index_entry_t *e = &x->index[k];
        uint8_t m[XFER_VERSION_MAX + 2];
        uint32_t n = 0;
        const size_t len = strlen(e->name); /* an asset name: room for the suffix */
        memcpy(meta, e->name, len);
        meta[len] = '@';
        meta[len + 1u] = '\0';
        if (b->stat(b->ctx, meta, &n) == BLOB_OK && n <= sizeof(m) && b->read(b->ctx, meta, 0, m, n) == BLOB_OK) {
            index_set_meta(e, m, n);
        }
    }
    i = 0;
    while (complete && b->list(b->ctx, i, name, sizeof(name), NULL) == BLOB_OK) {
        const size_t n = strlen(name);
        uint8_t type;
        const char *id;
        size_t id_len;
        bool drop = false;
        if (parse_asset_name(name, &type, &id, &id_len)) {
            drop = !index_has(x, name);
        } else if (n && name[n - 1u] == '@') { /* a version file without its asset */
            name[n - 1u] = '\0';
            drop = !index_has(x, name);
            name[n - 1u] = '@';
        }
        if (drop && b->remove(b->ctx, name) == BLOB_OK) {
            continue; /* the listing shifted down */
        }
        i++;
    }
    if (complete) {
        index_save(x);
    }
}

/* ---------------------------------------------------------------- journal */

static bool journal_load(const xfer_t *x, journal_t *j)
{
    uint8_t b[JOURNAL_MAX];
    size_t n = 0;
    if (x->p.kv->get(x->p.kv->ctx, XFER_JOURNAL_KEY, b, sizeof(b), &n) != KV_OK || n < JOURNAL_FIXED ||
        b[0] != 'J' || b[1] != JOURNAL_VERSION) {
        return false;
    }
    size_t id_len = b[52], ver_len = b[53];
    if (id_len == 0 || id_len > XFER_ID_MAX || ver_len > XFER_VERSION_MAX || n != JOURNAL_FIXED + id_len + ver_len ||
        (b[2] != PROTO_TRANSFER_KIND_ASSET && b[2] != PROTO_TRANSFER_KIND_FIRMWARE)) {
        return false;
    }
    memset(j, 0, sizeof(*j));
    j->t.kind = b[2];
    j->t.asset_type = b[3];
    j->t.size = proto_rd32(b + 4);
    j->t.crc32 = proto_rd32(b + 8);
    memcpy(j->t.sha256, b + 12, PROTO_SHA256_LEN);
    j->offset = proto_rd32(b + 44);
    j->prefix_crc = proto_rd32(b + 48);
    j->t.id_len = (uint8_t)id_len;
    j->t.ver_len = (uint8_t)ver_len;
    memcpy(j->t.id, b + JOURNAL_FIXED, id_len);
    memcpy(j->t.ver, b + JOURNAL_FIXED + id_len, ver_len);
    return j->offset <= j->t.size;
}

/* Record the session's offset. Asset data is flushed first so the journal never runs ahead of it. */
static void journal_write(xfer_t *x)
{
    const xfer_target_t *t = &x->s.t;
    x->s.journaled = x->s.offset;
    if (t->kind == PROTO_TRANSFER_KIND_ASSET && x->s.offset > 0) {
        char temp[BLOB_NAME_MAX + 1];
        target_name(t, '~', temp);
        if (x->p.blob->flush(x->p.blob->ctx, temp) != BLOB_OK) {
            return;
        }
    }
    uint8_t b[JOURNAL_MAX] = {'J', JOURNAL_VERSION, t->kind, t->asset_type};
    proto_wr32(b + 4, t->size);
    proto_wr32(b + 8, t->crc32);
    memcpy(b + 12, t->sha256, PROTO_SHA256_LEN);
    proto_wr32(b + 44, x->s.offset);
    proto_wr32(b + 48, x->s.crc);
    b[52] = t->id_len;
    b[53] = t->ver_len;
    memcpy(b + JOURNAL_FIXED, t->id, t->id_len);
    memcpy(b + JOURNAL_FIXED + t->id_len, t->ver, t->ver_len);
    (void)x->p.kv->set(x->p.kv->ctx, XFER_JOURNAL_KEY, b, JOURNAL_FIXED + t->id_len + t->ver_len);
}

static void journal_erase(xfer_t *x) { (void)x->p.kv->erase(x->p.kv->ctx, XFER_JOURNAL_KEY); }

/* Delete the stored data of an incomplete transfer and its journal. */
static void discard(xfer_t *x, const xfer_target_t *t)
{
    if (t->kind == PROTO_TRANSFER_KIND_ASSET) {
        char temp[BLOB_NAME_MAX + 1];
        target_name(t, '~', temp);
        (void)x->p.blob->remove(x->p.blob->ctx, temp);
    } else {
        x->p.fw->abort(x->p.fw->ctx);
    }
    journal_erase(x);
}

static void discard_journaled(xfer_t *x)
{
    journal_t j;
    if (journal_load(x, &j)) {
        discard(x, &j.t);
    }
}

/* ---------------------------------------------------------------- init / disconnect */

void xfer_init(xfer_t *x, const xfer_params_t *p)
{
    memset(x, 0, sizeof(*x));
    x->p = *p;
    if (x->p.max_chunk > PROTO_LIMIT_CHUNK_DATA_MAX) {
        x->p.max_chunk = PROTO_LIMIT_CHUNK_DATA_MAX;
    }
    x->next_handle = 1;
    if (!p->blob) {
        return;
    }
    /* Clean up after an interrupted transfer or install: keep only the journaled temp file. */
    journal_t j;
    char keep[BLOB_NAME_MAX + 1] = "", name[BLOB_NAME_MAX + 1];
    if (journal_load(x, &j) && j.t.kind == PROTO_TRANSFER_KIND_ASSET) {
        target_name(&j.t, '~', keep);
    }
    /* The saved index is trusted unless a change was cut off, and only if it names exactly the asset files there are
     * (a walk of names, no file is opened); otherwise it is rebuilt, which also drops orphaned version files. */
    uint8_t v;
    size_t v_len;
    x->index_dirty = p->index_store && p->kv->get(p->kv->ctx, XFER_INDEX_DIRTY_KEY, &v, 1, &v_len) != KV_NOT_FOUND;
    bool trust = !x->index_dirty && index_load(x);
    uint32_t i = 0, assets = 0;
    while (p->blob->list(p->blob->ctx, i, name, sizeof(name), NULL) == BLOB_OK) {
        const size_t n = strlen(name);
        uint8_t type;
        const char *id;
        size_t id_len;
        bool drop = false;
        if (n && name[n - 1] == '~') {
            drop = strcmp(name, keep) != 0;
        } else if (trust && parse_asset_name(name, &type, &id, &id_len)) {
            trust = index_has(x, name);
            assets++;
        } else if (trust && n && name[n - 1] == '@') {
            name[n - 1] = '\0';
            trust = index_has(x, name);
            name[n - 1] = '@';
        }
        if (drop && p->blob->remove(p->blob->ctx, name) == BLOB_OK) {
            continue; /* the listing shifted down */
        }
        i++;
    }
    if (!trust || assets != x->n_index) {
        index_rebuild(x);
    }
}

void xfer_on_disconnect(xfer_t *x)
{
    if (x->has_session && x->s.state == PROTO_XFER_STATE_RECEIVING) {
        journal_write(x);
    }
    x->has_session = false;
}

/* ---------------------------------------------------------------- helpers */

static void ack(xfer_t *x, uint8_t handle, uint32_t offset, proto_status_t st)
{
    uint8_t b[24];
    proto_writer_t w;
    proto_writer_init(&w, b, sizeof(b));
    proto_w_header(&w, PROTO_MSG_XFER_ACK, 0);
    proto_w_u8(&w, PROTO_TAG_XFER_ACK_HANDLE, handle);
    proto_w_u32(&w, PROTO_TAG_XFER_ACK_OFFSET, offset);
    proto_w_u8(&w, PROTO_TAG_XFER_ACK_STATUS, (uint8_t)st);
    if (x->p.emit) {
        x->p.emit(x->p.ctx, b, w.len);
    }
}

static bool session_is(const xfer_t *x, uint8_t handle)
{
    return x->has_session && x->s.handle == handle;
}

static bool same_target(const xfer_target_t *a, const xfer_target_t *b)
{
    return a->kind == b->kind && a->asset_type == b->asset_type && a->id_len == b->id_len &&
           memcmp(a->id, b->id, a->id_len) == 0;
}

static bool same_content(const xfer_target_t *a, const xfer_target_t *b)
{
    return same_target(a, b) && a->size == b->size && a->crc32 == b->crc32 &&
           memcmp(a->sha256, b->sha256, PROTO_SHA256_LEN) == 0;
}

static uint32_t asset_max_size(const xfer_t *x, uint8_t type, bool *supported)
{
    uint32_t max = 0;
    *supported = false;
    for (size_t i = 0; i < x->p.n_assets; i++) {
        if (x->p.assets[i].asset_type == type) {
            *supported = true;
            max = x->p.assets[i].max_size > max ? x->p.assets[i].max_size : max;
        }
    }
    return max;
}

/* Bytes and number of the installed assets of `type`, not counting the one named `skip` (it is being replaced). */
static void type_usage(const xfer_t *x, uint8_t type, const char *skip, uint32_t *bytes, uint16_t *count)
{
    *bytes = 0;
    *count = 0;
    for (uint16_t i = 0; i < x->n_index; i++) {
        uint8_t t;
        const char *id;
        size_t id_len;
        const xfer_index_entry_t *e = &x->index[i];
        if (parse_asset_name(e->name, &t, &id, &id_len) && t == type && strcmp(e->name, skip) != 0) {
            *bytes += e->size;
            (*count)++;
        }
    }
}

/* The local limits of an asset type (xfer_asset_support_t.budget, .max_count) and the room in the index: NO_SPACE when
 * the new file would break one. */
static proto_status_t check_budget(const xfer_t *x, const xfer_target_t *t)
{
    char name[BLOB_NAME_MAX + 1];
    target_name(t, 0, name);
    const bool replaces = index_has(x, name);
    if (!replaces && x->n_index >= XFER_INDEX_MAX) {
        return PROTO_NO_SPACE;
    }
    const xfer_asset_support_t *e = NULL;
    for (size_t i = 0; i < x->p.n_assets && !e; i++) {
        if (x->p.assets[i].asset_type == t->asset_type && (x->p.assets[i].budget || x->p.assets[i].max_count)) {
            e = &x->p.assets[i];
        }
    }
    if (!e) {
        return PROTO_OK;
    }
    uint32_t bytes;
    uint16_t count;
    type_usage(x, t->asset_type, name, &bytes, &count);
    if ((e->max_count && !replaces && count >= e->max_count) || (e->budget && (uint64_t)bytes + t->size > e->budget)) {
        return PROTO_NO_SPACE;
    }
    return PROTO_OK;
}

static proto_status_t blob_to_status(blob_status_t b)
{
    return b == BLOB_OK ? PROTO_OK : b == BLOB_NO_SPACE ? PROTO_NO_SPACE : PROTO_INTERNAL;
}

/* ---------------------------------------------------------------- XFER_BEGIN (protocol.md 7.2) */

static proto_status_t begin_checks(xfer_t *x, const xfer_target_t *t, bool has_type, bool has_version)
{
    if (x->has_session && x->s.state == PROTO_XFER_STATE_RECEIVING && !same_target(&x->s.t, t)) {
        return PROTO_BUSY;
    }
    uint32_t limit;
    if (t->kind == PROTO_TRANSFER_KIND_ASSET && x->p.blob && x->p.n_assets) {
        bool supported;
        if (!has_type) {
            return PROTO_MISSING_FIELD;
        }
        limit = asset_max_size(x, t->asset_type, &supported);
        if (!supported) {
            return PROTO_UNSUPPORTED;
        }
    } else if (t->kind == PROTO_TRANSFER_KIND_FIRMWARE && x->p.fw) {
        if (t->id_len != sizeof(FW_ID) - 1 || memcmp(t->id, FW_ID, t->id_len) != 0) {
            return PROTO_INVALID_ID;
        }
        limit = x->p.fw->slot_size;
    } else {
        return PROTO_UNSUPPORTED;
    }
    if (t->size > limit) {
        return PROTO_TOO_LARGE;
    }
    if (t->kind == PROTO_TRANSFER_KIND_ASSET) {
        /* the journaled transfer's bytes are reused or freed by this one */
        uint32_t total, free_bytes, held = 0;
        journal_t j;
        if (journal_load(x, &j) && j.t.kind == PROTO_TRANSFER_KIND_ASSET) {
            char temp[BLOB_NAME_MAX + 1];
            target_name(&j.t, '~', temp);
            if (x->p.blob->stat(x->p.blob->ctx, temp, &held) != BLOB_OK) {
                held = 0;
            }
        }
        if (x->p.blob->usage(x->p.blob->ctx, &total, &free_bytes) != BLOB_OK ||
            (uint64_t)t->size > (uint64_t)free_bytes + held) {
            return PROTO_NO_SPACE;
        }
        return check_budget(x, t);
    }
    return x->p.ota ? xfer_ota_check_begin(x->p.ota, t->ver, t->ver_len, has_version) : PROTO_OK;
}

static void handle_begin(xfer_t *x, const proto_msg_t *req, proto_writer_t *w)
{
    const uint8_t *body = req->body, *v;
    size_t len = req->body_len;
    uint16_t vl;
    xfer_target_t t;
    memset(&t, 0, sizeof(t));
    bool restart = false, has_version = false, has_type;
    (void)proto_get_u8(body, len, PROTO_TAG_XFER_BEGIN_KIND, &t.kind);
    (void)proto_find(body, len, PROTO_TAG_XFER_BEGIN_ID, &v, &vl); /* required, 1-32 bytes: validated */
    t.id_len = (uint8_t)vl;
    memcpy(t.id, v, vl);
    has_type = proto_get_u8(body, len, PROTO_TAG_XFER_BEGIN_ASSET_TYPE, &t.asset_type);
    (void)proto_get_u32(body, len, PROTO_TAG_XFER_BEGIN_SIZE, &t.size);
    (void)proto_get_u32(body, len, PROTO_TAG_XFER_BEGIN_CRC32, &t.crc32);
    (void)proto_find(body, len, PROTO_TAG_XFER_BEGIN_SHA256, &v, &vl); /* exactly 32 bytes: validated */
    memcpy(t.sha256, v, PROTO_SHA256_LEN);
    if (proto_find(body, len, PROTO_TAG_XFER_BEGIN_VERSION, &v, &vl)) { /* <= 32 bytes: validated */
        has_version = true;
        t.ver_len = (uint8_t)vl;
        memcpy(t.ver, v, vl);
    }
    (void)proto_get_bool(body, len, PROTO_TAG_XFER_BEGIN_RESTART, &restart);
    t.has_origin = t.kind == PROTO_TRANSFER_KIND_ASSET && proto_get_u8(body, len, PROTO_TAG_XFER_BEGIN_ORIGIN, &t.origin);

    proto_status_t st = begin_checks(x, &t, has_type, has_version);
    if (st != PROTO_OK) {
        proto_w_response(w, req, st);
        return;
    }
    xfer_on_disconnect(x); /* journal the current session, as a disconnect would */
    journal_t j;
    bool resume = journal_load(x, &j) && same_content(&j.t, &t) && !restart;
    char temp[BLOB_NAME_MAX + 1];
    if (t.kind == PROTO_TRANSFER_KIND_ASSET) {
        target_name(&t, '~', temp);
    }
    if (resume) { /* is the journaled data still there? */
        uint32_t stored = 0;
        resume = t.kind == PROTO_TRANSFER_KIND_ASSET
                     ? j.offset == 0 || (x->p.blob->stat(x->p.blob->ctx, temp, &stored) == BLOB_OK && stored >= j.offset)
                     : x->p.fw->resume(x->p.fw->ctx, t.size, j.offset) == PROTO_OK;
    }
    x->s.offset = resume ? j.offset : 0;
    x->s.crc = resume ? j.prefix_crc : 0;
    if (!resume) {
        discard_journaled(x);
        /* Stale data must go: a shorter new file would otherwise keep its tail. */
        st = t.kind == PROTO_TRANSFER_KIND_ASSET ? blob_to_status(x->p.blob->remove(x->p.blob->ctx, temp))
                                                 : x->p.fw->begin(x->p.fw->ctx, t.size);
        if (st != PROTO_OK) {
            /* BUSY from the firmware sink: another update (over Wi-Fi) is writing the image */
            proto_w_response(w, req, t.kind == PROTO_TRANSFER_KIND_FIRMWARE && st == PROTO_BUSY ? PROTO_BUSY : PROTO_INTERNAL);
            return;
        }
    }
    x->has_session = true;
    x->s.t = t;
    x->s.handle = x->next_handle;
    x->next_handle = (uint8_t)(x->next_handle % 255u + 1u);
    x->s.state = PROTO_XFER_STATE_RECEIVING;
    x->s.last_ack = x->s.offset;
    journal_write(x);

    proto_w_response(w, req, PROTO_OK);
    proto_w_u8(w, PROTO_TAG_XFER_BEGIN_RSP_HANDLE, x->s.handle);
    proto_w_u32(w, PROTO_TAG_XFER_BEGIN_RSP_OFFSET, x->s.offset);
    proto_w_u32(w, PROTO_TAG_XFER_BEGIN_RSP_PREFIX_CRC32, x->s.crc);
    proto_w_u16(w, PROTO_TAG_XFER_BEGIN_RSP_MAX_CHUNK, x->p.max_chunk);
    proto_w_u32(w, PROTO_TAG_XFER_BEGIN_RSP_ACK_WINDOW, x->p.ack_window);
}

/* ---------------------------------------------------------------- XFER_CHUNK (protocol.md 7.3) */

static void handle_chunk(xfer_t *x, const proto_msg_t *req)
{
    uint8_t handle = 0;
    uint32_t offset = 0;
    const uint8_t *data = NULL;
    uint16_t len = 0;
    (void)proto_get_u8(req->body, req->body_len, PROTO_TAG_XFER_CHUNK_HANDLE, &handle);
    (void)proto_get_u32(req->body, req->body_len, PROTO_TAG_XFER_CHUNK_OFFSET, &offset);
    (void)proto_find(req->body, req->body_len, PROTO_TAG_XFER_CHUNK_DATA, &data, &len);
    if (!session_is(x, handle) || x->s.state != PROTO_XFER_STATE_RECEIVING) {
        ack(x, handle, 0, PROTO_NO_SESSION);
        return;
    }
    if (offset != x->s.offset) {
        ack(x, handle, x->s.offset, PROTO_BAD_OFFSET);
        return;
    }
    if ((uint64_t)offset + len > x->s.t.size) {
        ack(x, handle, x->s.offset, PROTO_TOO_LARGE);
        return;
    }
    proto_status_t st;
    if (x->s.t.kind == PROTO_TRANSFER_KIND_ASSET) {
        char temp[BLOB_NAME_MAX + 1];
        target_name(&x->s.t, '~', temp);
        st = blob_to_status(x->p.blob->write(x->p.blob->ctx, temp, offset, data, len));
    } else {
        st = x->p.fw->write(x->p.fw->ctx, offset, data, len);
        st = st == PROTO_OK || st == PROTO_NO_SPACE ? st : PROTO_INTERNAL;
    }
    if (st != PROTO_OK) {
        ack(x, handle, x->s.offset, st);
        return;
    }
    x->s.offset += len;
    x->s.crc = proto_crc32_update(x->s.crc, data, len);
    if (x->s.t.kind == PROTO_TRANSFER_KIND_FIRMWARE && x->p.ota) {
        xfer_ota_progress(x->p.ota, x->s.offset, x->s.t.size);
    }
    bool done = x->s.offset == x->s.t.size;
    if (x->s.offset - x->s.journaled >= x->p.journal_every || done) {
        journal_write(x);
    }
    if (x->s.offset - x->s.last_ack >= x->p.ack_window || done) {
        x->s.last_ack = x->s.offset;
        ack(x, handle, x->s.offset, PROTO_OK);
    }
}

void xfer_chunk_invalid(xfer_t *x, const proto_msg_t *req, proto_status_t st)
{
    uint8_t handle = 0;
    (void)proto_get_u8(req->body, req->body_len, PROTO_TAG_XFER_CHUNK_HANDLE, &handle);
    bool live = session_is(x, handle) && x->s.state == PROTO_XFER_STATE_RECEIVING;
    ack(x, handle, live ? x->s.offset : 0, st);
}

/* ---------------------------------------------------------------- XFER_FINISH (protocol.md 7.4) */

static proto_status_t check_asset(const xfer_t *x, const uint8_t *h, uint32_t size)
{
    if (size < XFER_ASSET_HEADER_LEN) {
        return PROTO_IMAGE_INVALID;
    }
    uint8_t fmt = h[6];
    uint16_t width = proto_rd16(h + 8), height = proto_rd16(h + 10);
    uint32_t payload = proto_rd32(h + 12);
    const xfer_asset_support_t *entry = NULL;
    for (size_t i = 0; i < x->p.n_assets && !entry; i++) {
        if (x->p.assets[i].asset_type == x->s.t.asset_type && x->p.assets[i].format == fmt) {
            entry = &x->p.assets[i];
        }
    }
    if (memcmp(h, "ESPA", 4) != 0 || h[4] != 1 || h[5] != x->s.t.asset_type || h[7] != 0 || !entry ||
        payload != size - XFER_ASSET_HEADER_LEN) {
        return PROTO_IMAGE_INVALID;
    }
    /* images: the size the asset_support entry advertises (app_icon), otherwise the display size */
    bool fixed = entry->width || entry->height;
    uint16_t want_w = fixed ? entry->width : x->p.display_w, want_h = fixed ? entry->height : x->p.display_h;
    if (fmt == PROTO_ASSET_FORMAT_MONO1 &&
        (width != want_w || height != want_h || payload != (uint32_t)(width + 7u) / 8u * height)) {
        return PROTO_IMAGE_INVALID;
    }
    const uint32_t pixels = (uint32_t)width * height;
    if (fmt == PROTO_ASSET_FORMAT_RGB565 && (width != want_w || height != want_h || payload != 2u * pixels)) {
        return PROTO_IMAGE_INVALID;
    }
    if (fmt == PROTO_ASSET_FORMAT_RGB565A8 && (width != want_w || height != want_h || payload != 3u * pixels)) {
        return PROTO_IMAGE_INVALID;
    }
    if (fmt == PROTO_ASSET_FORMAT_FONT_V1 && (width != 0 || height != 0)) {
        return PROTO_IMAGE_INVALID;
    }
    return PROTO_OK;
}

void xfer_app_icon_id(const char *app_id, size_t len, char out[XFER_APP_ICON_ID_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t sha[PROTO_SHA256_LEN];
    proto_sha256(app_id, len, sha);
    for (size_t i = 0; i < XFER_APP_ICON_ID_LEN / 2u; i++) {
        out[2 * i] = hex[sha[i] >> 4];
        out[2 * i + 1] = hex[sha[i] & 0x0Fu];
    }
    out[XFER_APP_ICON_ID_LEN] = '\0';
}

/* CRC-32 and SHA-256 of the stored bytes, then the content check. */
static proto_status_t verify(xfer_t *x)
{
    const xfer_target_t *t = &x->s.t;
    char temp[BLOB_NAME_MAX + 1];
    uint8_t block[VERIFY_BLOCK], header[XFER_ASSET_HEADER_LEN], sha[PROTO_SHA256_LEN];
    proto_sha256_t ctx;
    uint32_t crc = 0;
    if (t->kind == PROTO_TRANSFER_KIND_ASSET) {
        uint32_t stored = 0;
        target_name(t, '~', temp);
        if (x->p.blob->stat(x->p.blob->ctx, temp, &stored) != BLOB_OK || stored != t->size) {
            return PROTO_CRC_MISMATCH; /* the stored file is not exactly the declared bytes */
        }
    }
    proto_sha256_init(&ctx);
    for (uint32_t pos = 0; pos < t->size;) {
        size_t n = t->size - pos < VERIFY_BLOCK ? t->size - pos : VERIFY_BLOCK;
        bool ok = t->kind == PROTO_TRANSFER_KIND_ASSET
                      ? x->p.blob->read(x->p.blob->ctx, temp, pos, block, n) == BLOB_OK
                      : x->p.fw->read(x->p.fw->ctx, pos, block, n) == PROTO_OK;
        if (!ok) {
            return PROTO_INTERNAL;
        }
        crc = proto_crc32_update(crc, block, n);
        proto_sha256_update(&ctx, block, n);
        if (pos < XFER_ASSET_HEADER_LEN) {
            memcpy(header + pos, block, n < XFER_ASSET_HEADER_LEN - pos ? n : XFER_ASSET_HEADER_LEN - pos);
        }
        pos += (uint32_t)n;
    }
    proto_sha256_final(&ctx, sha);
    if (crc != t->crc32) {
        return PROTO_CRC_MISMATCH;
    }
    if (memcmp(sha, t->sha256, PROTO_SHA256_LEN) != 0) {
        return PROTO_HASH_MISMATCH;
    }
    if (t->kind == PROTO_TRANSFER_KIND_ASSET) {
        const proto_status_t st = check_asset(x, header, t->size);
        if (st == PROTO_OK && x->p.check_asset) {
            return x->p.check_asset(x->p.ctx, t->asset_type, t->id, t->id_len, temp, t->size);
        }
        return st;
    }
    return x->p.fw->finish(x->p.fw->ctx, t->size);
}

/* temp -> final, atomically replacing a previous version; the version file is best effort. The index follows the
 * files and is saved only when it is known to match them (else the next boot rebuilds it). */
static proto_status_t install_asset(xfer_t *x)
{
    const xfer_target_t *t = &x->s.t;
    const blob_storage_t *b = x->p.blob;
    char temp[BLOB_NAME_MAX + 1], final[BLOB_NAME_MAX + 1], meta[BLOB_NAME_MAX + 1];
    target_name(t, '~', temp);
    target_name(t, 0, final);
    target_name(t, '@', meta);
    index_mark_dirty(x);
    bool exact = b->remove(b->ctx, meta) == BLOB_OK;
    xfer_index_entry_t *e = index_get(x, final);
    if (exact && e) {
        index_set_meta(e, NULL, 0);
    }
    if (b->rename(b->ctx, temp, final) != BLOB_OK) {
        return PROTO_INTERNAL;
    }
    e = index_put(x, final, t->size);
    exact = exact && e;
    if (t->ver_len || t->has_origin) { /* the version, then NUL and the origin when the BEGIN carried one */
        uint8_t m[XFER_VERSION_MAX + 2];
        size_t n = t->ver_len;
        memcpy(m, t->ver, n);
        if (t->has_origin) {
            m[n++] = 0;
            m[n++] = t->origin;
        }
        exact = exact && b->write(b->ctx, meta, 0, m, n) == BLOB_OK && b->flush(b->ctx, meta) == BLOB_OK;
        if (exact) {
            index_set_meta(e, m, (uint32_t)n);
        }
    }
    if (exact) {
        index_save(x);
    }
    if (x->p.on_asset) {
        x->p.on_asset(x->p.ctx, t->asset_type);
    }
    return PROTO_OK;
}

static proto_status_t finish(xfer_t *x, uint8_t handle)
{
    if (!session_is(x, handle) || x->s.state != PROTO_XFER_STATE_RECEIVING) {
        return PROTO_NO_SESSION;
    }
    if (x->s.offset < x->s.t.size) {
        return PROTO_BAD_OFFSET;
    }
    bool firmware = x->s.t.kind == PROTO_TRANSFER_KIND_FIRMWARE;
    proto_status_t st;
    if (firmware && x->p.ota && (st = xfer_ota_check_battery(x->p.ota)) != PROTO_OK) {
        return st;
    }
    st = verify(x);
    if (st != PROTO_OK) {
        discard(x, &x->s.t);
        x->s.state = PROTO_XFER_STATE_FAILED;
        return st;
    }
    if (!firmware && (st = install_asset(x)) != PROTO_OK) {
        return st; /* still receiving: FINISH can be retried */
    }
    journal_erase(x);
    x->s.state = PROTO_XFER_STATE_COMPLETE;
    if (firmware && x->p.ota) {
        xfer_ota_installed(x->p.ota, x->s.t.ver, x->s.t.ver_len);
    }
    return PROTO_OK;
}

/* ---------------------------------------------------------------- storage requests (protocol.md 7.5) */

static void handle_storage_info(const xfer_t *x, const proto_msg_t *req, proto_writer_t *w)
{
    const blob_storage_t *b = x->p.blob;
    uint32_t total, free_bytes;
    if (!b || b->usage(b->ctx, &total, &free_bytes) != BLOB_OK) {
        proto_w_response(w, req, b ? PROTO_INTERNAL : PROTO_UNSUPPORTED);
        return;
    }
    proto_w_response(w, req, PROTO_OK);
    proto_w_u32(w, PROTO_TAG_STORAGE_INFO_RSP_TOTAL, total);
    proto_w_u32(w, PROTO_TAG_STORAGE_INFO_RSP_FREE, free_bytes);
    proto_w_u16(w, PROTO_TAG_STORAGE_INFO_RSP_ASSET_COUNT, x->n_index);
}

/* From the index: no file is touched. */
static void handle_asset_list(const xfer_t *x, const proto_msg_t *req, proto_writer_t *w)
{
    if (!x->p.blob) {
        proto_w_response(w, req, PROTO_UNSUPPORTED);
        return;
    }
    uint16_t offset = 0;
    uint8_t limit = LIST_DEFAULT_LIMIT;
    (void)proto_get_u16(req->body, req->body_len, PROTO_TAG_ASSET_LIST_OFFSET, &offset);
    (void)proto_get_u8(req->body, req->body_len, PROTO_TAG_ASSET_LIST_LIMIT, &limit);
    proto_w_response(w, req, PROTO_OK);
    proto_w_u16(w, PROTO_TAG_ASSET_LIST_RSP_TOTAL, x->n_index);

    uint16_t written = 0;
    for (uint32_t k = offset; k < x->n_index && written < limit; k++) {
        const xfer_index_entry_t *e = &x->index[k];
        uint8_t type;
        const char *id;
        size_t id_len;
        (void)parse_asset_name(e->name, &type, &id, &id_len); /* index names are asset names */
        size_t entry = PROTO_TLV_HDR_LEN + (PROTO_TLV_HDR_LEN + 1) + (PROTO_TLV_HDR_LEN + id_len) +
                       (PROTO_TLV_HDR_LEN + 4) + (e->ver_len ? PROTO_TLV_HDR_LEN + e->ver_len : 0) +
                       (e->has_origin ? PROTO_TLV_HDR_LEN + 1 : 0);
        if (w->len + entry > PROTO_LIMIT_MAX_MESSAGE_TRANSFER) {
            break; /* the app pages on (protocol.md 5) */
        }
        proto_w_struct_begin(w, PROTO_TAG_ASSET_LIST_RSP_ASSET);
        proto_w_u8(w, PROTO_TAG_ASSET_ENTRY_ASSET_TYPE, type);
        proto_w_bytes(w, PROTO_TAG_ASSET_ENTRY_ID, id, id_len);
        proto_w_u32(w, PROTO_TAG_ASSET_ENTRY_SIZE, e->size);
        if (e->ver_len) {
            proto_w_bytes(w, PROTO_TAG_ASSET_ENTRY_VERSION, e->ver, e->ver_len);
        }
        if (e->has_origin) {
            proto_w_u8(w, PROTO_TAG_ASSET_ENTRY_ORIGIN, e->origin);
        }
        proto_w_struct_end(w);
        written++;
    }
}

proto_status_t xfer_asset_remove(xfer_t *x, uint8_t type, const char *id, size_t id_len)
{
    const blob_storage_t *b = x->p.blob;
    if (!b) {
        return PROTO_UNSUPPORTED;
    }
    uint32_t size;
    char name[BLOB_NAME_MAX + 1], meta[BLOB_NAME_MAX + 1];
    file_name(type, id, id_len, 0, name);
    file_name(type, id, id_len, '@', meta);
    if (!index_has(x, name) && b->stat(b->ctx, name, &size) != BLOB_OK) { /* the file too: one the index missed */
        return PROTO_NOT_FOUND;
    }
    index_mark_dirty(x);
    if (b->remove(b->ctx, name) != BLOB_OK) {
        return PROTO_INTERNAL;
    }
    index_drop(x, name);
    if (b->remove(b->ctx, meta) == BLOB_OK) {
        index_save(x);
    }
    if (x->p.on_asset) {
        x->p.on_asset(x->p.ctx, type);
    }
    return PROTO_OK;
}

static proto_status_t asset_delete(xfer_t *x, const proto_msg_t *req)
{
    uint8_t type = 0;
    const uint8_t *id;
    uint16_t id_len;
    (void)proto_get_u8(req->body, req->body_len, PROTO_TAG_ASSET_DELETE_ASSET_TYPE, &type);
    (void)proto_find(req->body, req->body_len, PROTO_TAG_ASSET_DELETE_ID, &id, &id_len); /* validated */
    return xfer_asset_remove(x, type, (const char *)id, id_len);
}

/* ---------------------------------------------------------------- dispatch */

bool xfer_handle(xfer_t *x, const proto_msg_t *req, proto_writer_t *w)
{
    uint8_t handle = 0;
    (void)proto_get_u8(req->body, req->body_len, 1, &handle); /* tag 1 = handle in CHUNK/STATUS/FINISH/ABORT */
    switch (req->type) {
    case PROTO_MSG_XFER_BEGIN:
        handle_begin(x, req, w);
        return true;
    case PROTO_MSG_XFER_CHUNK:
        handle_chunk(x, req);
        return false;
    case PROTO_MSG_XFER_STATUS:
        proto_w_response(w, req, session_is(x, handle) ? PROTO_OK : PROTO_NO_SESSION);
        if (session_is(x, handle)) {
            proto_w_u8(w, PROTO_TAG_XFER_STATUS_RSP_STATE, x->s.state);
            proto_w_u32(w, PROTO_TAG_XFER_STATUS_RSP_OFFSET, x->s.offset);
            proto_w_u32(w, PROTO_TAG_XFER_STATUS_RSP_PREFIX_CRC32, x->s.crc);
        }
        return true;
    case PROTO_MSG_XFER_FINISH:
        proto_w_response(w, req, finish(x, handle));
        return true;
    case PROTO_MSG_XFER_ABORT:
        if (!session_is(x, handle)) {
            proto_w_response(w, req, PROTO_NO_SESSION);
            return true;
        }
        if (x->s.state == PROTO_XFER_STATE_RECEIVING) {
            discard(x, &x->s.t);
        }
        x->has_session = false;
        proto_w_response(w, req, PROTO_OK);
        return true;
    case PROTO_MSG_STORAGE_INFO:
        handle_storage_info(x, req, w);
        return true;
    case PROTO_MSG_ASSET_LIST:
        handle_asset_list(x, req, w);
        return true;
    case PROTO_MSG_ASSET_DELETE:
        proto_w_response(w, req, asset_delete(x, req));
        return true;
    default:
        return false;
    }
}
