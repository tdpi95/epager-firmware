#include "epager/cfg_store.h"

#include <string.h>

/* Stored record (internal, not part of the contract):
 *   "EC" | format u8 = 1 | 0 | contract version u32 (packed) | revision u32 |
 *   entries (key u16, len u8, value) for every non-default value | crc32 u32 of all the above.
 * Only non-default values are stored, so a later firmware's new default applies to keys the
 * user never changed. */
#define BLOB_FORMAT 1u
#define BLOB_HDR 12u
#define BLOB_VAL_MAX (PROTO_CFG_STR_MAX_LEN > 4u ? PROTO_CFG_STR_MAX_LEN : 4u)
#define BLOB_MAX CFG_BLOB_MAX
_Static_assert(BLOB_MAX == BLOB_HDR + PROTO_CFG_KEY_COUNT * (3u + BLOB_VAL_MAX) + 4u, "CFG_BLOB_MAX");

static int key_index(uint16_t key)
{
    for (size_t i = 0; i < proto_cfg_keys_count; i++) {
        if (proto_cfg_keys[i].id == key) {
            return (int)i;
        }
    }
    return -1;
}

static bool applies(const cfg_store_t *c, size_t i) { return proto_cfg_key_applies(&proto_cfg_keys[i], c->p.variant); }

/* Values are kept zero-padded so they compare with memcmp. */
static void set_default(const proto_cfg_key_desc_t *k, cfg_value_t *v)
{
    memset(v, 0, sizeof(*v));
    if (k->type == PROTO_FT_STR) {
        v->len = (uint8_t)strlen(k->default_str);
        memcpy(v->s, k->default_str, v->len);
    } else {
        v->i = k->default_int;
    }
}

/* val must already be valid for k (proto_cfg_validate). */
static void decode(const proto_cfg_key_desc_t *k, const uint8_t *val, size_t len, cfg_value_t *v)
{
    memset(v, 0, sizeof(*v));
    switch (k->type) {
    case PROTO_FT_STR:
    case PROTO_FT_BYTES: v->len = (uint8_t)len; memcpy(v->s, val, len); break;
    case PROTO_FT_U16: v->i = proto_rd16(val); break;
    case PROTO_FT_I16: v->i = (int16_t)proto_rd16(val); break;
    case PROTO_FT_U32:
    case PROTO_FT_I32: v->i = (int32_t)proto_rd32(val); break;
    default: v->i = val[0]; break; /* u8, bool */
    }
}

static size_t encode(const proto_cfg_key_desc_t *k, const cfg_value_t *v, uint8_t *out)
{
    switch (k->type) {
    case PROTO_FT_STR:
    case PROTO_FT_BYTES: memcpy(out, v->s, v->len); return v->len;
    case PROTO_FT_U16:
    case PROTO_FT_I16: proto_wr16(out, (uint16_t)v->i); return 2;
    case PROTO_FT_U32:
    case PROTO_FT_I32: proto_wr32(out, (uint32_t)v->i); return 4;
    default: out[0] = (uint8_t)v->i; return 1;
    }
}

static void load_blob(cfg_store_t *c, const uint8_t *b, size_t n)
{
    if (n < BLOB_HDR + 4 || b[0] != 'E' || b[1] != 'C' || b[2] != BLOB_FORMAT ||
        proto_crc32_update(0, b, n - 4) != proto_rd32(b + n - 4)) {
        return; /* missing or corrupt: defaults, revision 0 */
    }
    uint32_t stored_version = proto_rd32(b + 4);
    c->revision = proto_rd32(b + 8);
    for (size_t pos = BLOB_HDR; pos + 3 <= n - 4;) {
        uint16_t key = proto_rd16(b + pos);
        size_t len = b[pos + 2];
        pos += 3;
        if (pos + len > n - 4) {
            break;
        }
        int i = key_index(key);
        /* Dropped: keys this firmware does not know, keys not on this variant, keys newer than the
         * record (their stored bytes predate the key's meaning), and values that no longer validate. */
        if (i >= 0 && proto_cfg_keys[i].since <= stored_version &&
            proto_cfg_validate(key, b + pos, len, c->p.variant) == PROTO_OK) {
            decode(&proto_cfg_keys[i], b + pos, len, &c->v[i]);
        }
        pos += len;
    }
}

void cfg_init(cfg_store_t *c, const cfg_params_t *p)
{
    memset(c, 0, sizeof(*c));
    c->p = *p;
    for (size_t i = 0; i < proto_cfg_keys_count; i++) {
        set_default(&proto_cfg_keys[i], &c->v[i]);
    }
    size_t n = 0;
    if (p->kv->get(p->kv->ctx, CFG_KV_KEY, c->blob, sizeof(c->blob), &n) == KV_OK) {
        load_blob(c, c->blob, n);
    }
}

static proto_status_t commit(cfg_store_t *c, const cfg_value_t *nv, uint32_t revision)
{
    uint8_t *b = c->blob;
    b[0] = 'E';
    b[1] = 'C';
    b[2] = BLOB_FORMAT;
    b[3] = 0;
    proto_wr32(b + 4, PROTO_CONTRACT_VERSION_PACKED);
    proto_wr32(b + 8, revision);
    size_t n = BLOB_HDR;
    for (size_t i = 0; i < proto_cfg_keys_count; i++) {
        cfg_value_t d;
        set_default(&proto_cfg_keys[i], &d);
        if (memcmp(&d, &nv[i], sizeof(d)) == 0) {
            continue;
        }
        proto_wr16(b + n, proto_cfg_keys[i].id);
        b[n + 2] = (uint8_t)encode(&proto_cfg_keys[i], &nv[i], b + n + 3);
        n += 3u + b[n + 2];
    }
    proto_wr32(b + n, proto_crc32_update(0, b, n));
    n += 4;
    if (c->p.kv->set(c->p.kv->ctx, CFG_KV_KEY, b, n) != KV_OK) {
        return PROTO_INTERNAL;
    }
    if (nv != c->v) {
        memcpy(c->v, nv, sizeof(c->v));
    }
    c->revision = revision;
    return PROTO_OK;
}

/* Commit nv if it differs from the current values; then notify. */
static proto_status_t apply(cfg_store_t *c, const cfg_value_t *nv)
{
    uint16_t keys[PROTO_CFG_KEY_COUNT];
    size_t n = 0;
    for (size_t i = 0; i < proto_cfg_keys_count; i++) {
        if (memcmp(&nv[i], &c->v[i], sizeof(nv[i])) != 0) {
            keys[n++] = proto_cfg_keys[i].id; /* table is in ascending id order */
        }
    }
    if (n == 0) {
        return PROTO_OK;
    }
    proto_status_t st = commit(c, nv, c->revision + 1);
    if (st == PROTO_OK && c->p.on_change) {
        c->p.on_change(c->p.ctx, c->revision, keys, n);
    }
    return st;
}

uint32_t cfg_revision(const cfg_store_t *c) { return c->revision; }

int32_t cfg_get_int(const cfg_store_t *c, uint16_t key)
{
    int i = key_index(key);
    return i < 0 ? 0 : c->v[i].i;
}

size_t cfg_get_str(const cfg_store_t *c, uint16_t key, const uint8_t **s)
{
    int i = key_index(key);
    *s = i < 0 ? (const uint8_t *)"" : c->v[i].s;
    return i < 0 ? 0 : c->v[i].len;
}

bool cfg_encode_value(const cfg_store_t *c, uint16_t key, uint8_t *out, size_t *len)
{
    int i = key_index(key);
    if (i < 0 || !applies(c, (size_t)i)) {
        return false;
    }
    *len = encode(&proto_cfg_keys[i], &c->v[i], out);
    return true;
}

proto_status_t cfg_set(cfg_store_t *c, const cfg_entry_t *e, size_t n, const uint32_t *expected_revision,
                       proto_status_t *key_status)
{
    if (n > CFG_MAX_BATCH) {
        return PROTO_TOO_LARGE;
    }
    proto_status_t ks[CFG_MAX_BATCH];
    proto_status_t first = PROTO_OK;
    for (size_t i = 0; i < n; i++) {
        ks[i] = proto_cfg_validate(e[i].key, e[i].val, e[i].len, c->p.variant);
        if (ks[i] != PROTO_OK && ks[i] != PROTO_UNKNOWN_KEY && first == PROTO_OK) {
            first = ks[i];
        }
    }
    if (first != PROTO_OK) {
        return first;
    }
    if (expected_revision && *expected_revision != c->revision) {
        return PROTO_REVISION_CONFLICT;
    }
    cfg_value_t *nv = c->nv;
    memcpy(nv, c->v, sizeof(c->nv));
    for (size_t i = 0; i < n; i++) {
        if (ks[i] == PROTO_OK) {
            int k = key_index(e[i].key);
            decode(&proto_cfg_keys[k], e[i].val, e[i].len, &nv[k]);
        }
    }
    proto_status_t st = apply(c, nv);
    if (st == PROTO_OK && key_status) {
        memcpy(key_status, ks, n * sizeof(ks[0]));
    }
    return st;
}

proto_status_t cfg_reset(cfg_store_t *c)
{
    for (size_t i = 0; i < proto_cfg_keys_count; i++) {
        set_default(&proto_cfg_keys[i], &c->nv[i]);
    }
    return apply(c, c->nv);
}

proto_status_t cfg_bump_revision(cfg_store_t *c)
{
    proto_status_t st = commit(c, c->v, c->revision + 1);
    if (st == PROTO_OK && c->p.on_change) {
        c->p.on_change(c->p.ctx, c->revision, NULL, 0);
    }
    return st;
}

/* ---------------------------------------------------------------- message adapter */

static void write_entry(const cfg_store_t *c, proto_writer_t *w, size_t i)
{
    uint8_t val[BLOB_VAL_MAX];
    proto_w_struct_begin(w, PROTO_TAG_CONFIG_GET_RSP_ENTRY);
    proto_w_u16(w, PROTO_TAG_CONFIG_ENTRY_KEY, proto_cfg_keys[i].id);
    proto_w_bytes(w, PROTO_TAG_CONFIG_ENTRY_VALUE, val, encode(&proto_cfg_keys[i], &c->v[i], val));
    proto_w_struct_end(w);
}

static void handle_get(const cfg_store_t *c, const proto_msg_t *req, proto_writer_t *w)
{
    proto_w_response(w, req, PROTO_OK);
    proto_w_u32(w, PROTO_TAG_CONFIG_GET_RSP_REVISION, c->revision);
    bool seen[PROTO_CFG_KEY_COUNT] = {false}, any = false;
    size_t pos = 0;
    const uint8_t *v;
    uint16_t vl;
    while (proto_find_next(req->body, req->body_len, PROTO_TAG_CONFIG_GET_KEY, &pos, &v, &vl)) {
        any = true;
        int i = key_index(proto_rd16(v));
        if (i >= 0 && applies(c, (size_t)i) && !seen[i]) {
            seen[i] = true;
            write_entry(c, w, (size_t)i);
        }
    }
    for (size_t i = 0; !any && i < proto_cfg_keys_count; i++) {
        if (applies(c, i)) {
            write_entry(c, w, i);
        }
    }
}

static void handle_set(cfg_store_t *c, const proto_msg_t *req, proto_writer_t *w)
{
    cfg_entry_t e[CFG_MAX_BATCH];
    proto_status_t ks[CFG_MAX_BATCH];
    size_t n = 0, pos = 0;
    const uint8_t *v;
    uint16_t vl;
    while (proto_find_next(req->body, req->body_len, PROTO_TAG_CONFIG_SET_ENTRY, &pos, &v, &vl)) {
        if (n == CFG_MAX_BATCH) {
            proto_w_response(w, req, PROTO_TOO_LARGE);
            return;
        }
        uint16_t evl;
        (void)proto_get_u16(v, vl, PROTO_TAG_CONFIG_ENTRY_KEY, &e[n].key); /* required: validated */
        (void)proto_find(v, vl, PROTO_TAG_CONFIG_ENTRY_VALUE, &e[n].val, &evl);
        e[n++].len = evl;
    }
    uint32_t expected;
    bool has_expected = proto_get_u32(req->body, req->body_len, PROTO_TAG_CONFIG_SET_EXPECTED_REVISION, &expected);
    proto_status_t st = cfg_set(c, e, n, has_expected ? &expected : NULL, ks);
    proto_w_response(w, req, st);
    if (st != PROTO_OK) {
        return;
    }
    proto_w_u32(w, PROTO_TAG_CONFIG_SET_RSP_REVISION, c->revision);
    for (size_t i = 0; i < n; i++) {
        proto_w_struct_begin(w, PROTO_TAG_CONFIG_SET_RSP_KEY_STATUS);
        proto_w_u16(w, PROTO_TAG_KEY_STATUS_KEY, e[i].key);
        proto_w_u8(w, PROTO_TAG_KEY_STATUS_STATUS, (uint8_t)ks[i]);
        proto_w_struct_end(w);
    }
}

static void handle_time_sync(cfg_store_t *c, const proto_msg_t *req, proto_writer_t *w)
{
    uint32_t utc = 0;
    uint8_t tz[2] = {0, 0};
    const uint8_t *v;
    uint16_t vl;
    (void)proto_get_u32(req->body, req->body_len, PROTO_TAG_TIME_SYNC_UTC_S, &utc);
    if (proto_find(req->body, req->body_len, PROTO_TAG_TIME_SYNC_TZ_OFFSET_MIN, &v, &vl)) {
        memcpy(tz, v, sizeof(tz));
    }
    if (c->p.set_time) {
        c->p.set_time(c->p.ctx, utc);
    }
    cfg_entry_t e = {PROTO_CFG_TIME_TZ_OFFSET_MIN, tz, sizeof(tz)};
    proto_w_response(w, req, cfg_set(c, &e, 1, NULL, NULL));
}

bool cfg_handle(cfg_store_t *c, const proto_msg_t *req, proto_writer_t *w)
{
    proto_status_t st;
    switch (req->type) {
    case PROTO_MSG_CONFIG_GET:
        handle_get(c, req, w);
        return true;
    case PROTO_MSG_CONFIG_SET:
        handle_set(c, req, w);
        return true;
    case PROTO_MSG_CONFIG_RESET:
        st = cfg_reset(c);
        proto_w_response(w, req, st);
        if (st == PROTO_OK) {
            proto_w_u32(w, PROTO_TAG_CONFIG_RESET_RSP_REVISION, c->revision);
        }
        return true;
    case PROTO_MSG_TIME_SYNC:
        handle_time_sync(c, req, w);
        return true;
    default:
        return false;
    }
}

void cfg_write_changed_event(proto_writer_t *w, uint32_t revision, const uint16_t *keys, size_t n_keys)
{
    proto_w_header(w, PROTO_MSG_CONFIG_CHANGED, 0);
    proto_w_u32(w, PROTO_TAG_CONFIG_CHANGED_REVISION, revision);
    for (size_t i = 0; i < n_keys; i++) {
        proto_w_u16(w, PROTO_TAG_CONFIG_CHANGED_KEY, keys[i]);
    }
}
