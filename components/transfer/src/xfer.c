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
    uint32_t i = 0, size;
    while (p->blob->list(p->blob->ctx, i, name, sizeof(name), &size) == BLOB_OK) {
        size_t n = strlen(name);
        bool drop = false;
        if (n && name[n - 1] == '~') {
            drop = strcmp(name, keep) != 0;
        } else if (n && name[n - 1] == '@') {
            name[n - 1] = '\0'; /* version file without its asset */
            drop = p->blob->stat(p->blob->ctx, name, &size) != BLOB_OK;
            name[n - 1] = '@';
        }
        if (drop && p->blob->remove(p->blob->ctx, name) == BLOB_OK) {
            continue; /* the listing shifted down */
        }
        i++;
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
        return PROTO_OK;
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
        return check_asset(x, header, t->size);
    }
    return x->p.fw->finish(x->p.fw->ctx, t->size);
}

/* temp -> final, atomically replacing a previous version; the version file is best effort. */
static proto_status_t install_asset(xfer_t *x)
{
    const xfer_target_t *t = &x->s.t;
    const blob_storage_t *b = x->p.blob;
    char temp[BLOB_NAME_MAX + 1], final[BLOB_NAME_MAX + 1], meta[BLOB_NAME_MAX + 1];
    target_name(t, '~', temp);
    target_name(t, 0, final);
    target_name(t, '@', meta);
    (void)b->remove(b->ctx, meta);
    if (b->rename(b->ctx, temp, final) != BLOB_OK) {
        return PROTO_INTERNAL;
    }
    if (t->ver_len) {
        (void)b->write(b->ctx, meta, 0, t->ver, t->ver_len);
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

static uint16_t count_assets(const blob_storage_t *b)
{
    char name[BLOB_NAME_MAX + 1];
    uint32_t i = 0, size;
    uint16_t n = 0;
    while (b->list(b->ctx, i++, name, sizeof(name), &size) == BLOB_OK) {
        uint8_t type;
        const char *id;
        size_t id_len;
        n += parse_asset_name(name, &type, &id, &id_len);
    }
    return n;
}

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
    proto_w_u16(w, PROTO_TAG_STORAGE_INFO_RSP_ASSET_COUNT, count_assets(b));
}

static void handle_asset_list(const xfer_t *x, const proto_msg_t *req, proto_writer_t *w)
{
    const blob_storage_t *b = x->p.blob;
    if (!b) {
        proto_w_response(w, req, PROTO_UNSUPPORTED);
        return;
    }
    uint16_t offset = 0;
    uint8_t limit = LIST_DEFAULT_LIMIT;
    (void)proto_get_u16(req->body, req->body_len, PROTO_TAG_ASSET_LIST_OFFSET, &offset);
    (void)proto_get_u8(req->body, req->body_len, PROTO_TAG_ASSET_LIST_LIMIT, &limit);
    proto_w_response(w, req, PROTO_OK);
    proto_w_u16(w, PROTO_TAG_ASSET_LIST_RSP_TOTAL, count_assets(b));

    char name[BLOB_NAME_MAX + 1], meta[BLOB_NAME_MAX + 1];
    uint32_t i = 0, size;
    uint16_t index = 0, written = 0;
    while (written < limit && b->list(b->ctx, i++, name, sizeof(name), &size) == BLOB_OK) {
        uint8_t type, ver[XFER_VERSION_MAX];
        const char *id;
        size_t id_len;
        uint32_t ver_len = 0;
        if (!parse_asset_name(name, &type, &id, &id_len) || index++ < offset) {
            continue;
        }
        file_name(type, id, id_len, '@', meta);
        bool has_ver = b->stat(b->ctx, meta, &ver_len) == BLOB_OK && ver_len <= XFER_VERSION_MAX &&
                       b->read(b->ctx, meta, 0, ver, ver_len) == BLOB_OK;
        size_t entry = PROTO_TLV_HDR_LEN + (PROTO_TLV_HDR_LEN + 1) + (PROTO_TLV_HDR_LEN + id_len) +
                       (PROTO_TLV_HDR_LEN + 4) + (has_ver ? PROTO_TLV_HDR_LEN + ver_len : 0);
        if (w->len + entry > PROTO_LIMIT_MAX_MESSAGE_TRANSFER) {
            break; /* the app pages on (protocol.md 5) */
        }
        proto_w_struct_begin(w, PROTO_TAG_ASSET_LIST_RSP_ASSET);
        proto_w_u8(w, PROTO_TAG_ASSET_ENTRY_ASSET_TYPE, type);
        proto_w_bytes(w, PROTO_TAG_ASSET_ENTRY_ID, id, id_len);
        proto_w_u32(w, PROTO_TAG_ASSET_ENTRY_SIZE, size);
        if (has_ver) {
            proto_w_bytes(w, PROTO_TAG_ASSET_ENTRY_VERSION, ver, ver_len);
        }
        proto_w_struct_end(w);
        written++;
    }
}

proto_status_t xfer_asset_remove(const xfer_t *x, uint8_t type, const char *id, size_t id_len)
{
    const blob_storage_t *b = x->p.blob;
    if (!b) {
        return PROTO_UNSUPPORTED;
    }
    uint32_t size;
    char name[BLOB_NAME_MAX + 1], meta[BLOB_NAME_MAX + 1];
    file_name(type, id, id_len, 0, name);
    file_name(type, id, id_len, '@', meta);
    if (b->stat(b->ctx, name, &size) != BLOB_OK) {
        return PROTO_NOT_FOUND;
    }
    if (b->remove(b->ctx, name) != BLOB_OK) {
        return PROTO_INTERNAL;
    }
    (void)b->remove(b->ctx, meta);
    if (x->p.on_asset) {
        x->p.on_asset(x->p.ctx, type);
    }
    return PROTO_OK;
}

static proto_status_t asset_delete(const xfer_t *x, const proto_msg_t *req)
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
