/* Firmware sink over a sequential flash (portable). */
#include "epager/ota.h"

static proto_status_t from_flash(ota_flash_status_t st)
{
    switch (st) {
    case OTA_FLASH_OK:
        return PROTO_OK;
    case OTA_FLASH_NO_SPACE:
        return PROTO_NO_SPACE;
    case OTA_FLASH_BUSY:
        return PROTO_BUSY;
    case OTA_FLASH_BAD_IMAGE:
        return PROTO_IMAGE_INVALID;
    default:
        return PROTO_INTERNAL;
    }
}

/* Take the sink for v->who (or confirm it is already ours). The only place the owner changes hands. */
static bool claim(const ota_view_t *v)
{
    unsigned char none = OTA_OWNER_NONE;
    return atomic_compare_exchange_strong(&v->sink->owner, &none, (unsigned char)v->who) || none == v->who;
}

static bool owns(const ota_view_t *v) { return atomic_load(&v->sink->owner) == v->who; }

/* The status for a call by a path that does not own the sink: nobody does (no session), or the other path. */
static proto_status_t not_owner(const ota_view_t *v)
{
    return atomic_load(&v->sink->owner) == OTA_OWNER_NONE ? PROTO_NO_SESSION : PROTO_BUSY;
}

static void release(const ota_view_t *v)
{
    unsigned char me = (unsigned char)v->who;
    atomic_compare_exchange_strong(&v->sink->owner, &me, OTA_OWNER_NONE);
}

static proto_status_t sink_begin(void *ctx, uint32_t size)
{
    const ota_view_t *v = ctx;
    ota_sink_t *s = v->sink;
    if (size > s->flash->slot_size) {
        return PROTO_NO_SPACE;
    }
    if (!claim(v)) {
        return PROTO_BUSY;
    }
    s->open = false;
    s->written = 0;
    proto_status_t st = from_flash(s->flash->begin(s->flash->ctx));
    s->open = st == PROTO_OK;
    if (!s->open) {
        release(v);
    }
    return st;
}

static proto_status_t sink_resume(void *ctx, uint32_t size, uint32_t offset)
{
    const ota_view_t *v = ctx;
    ota_sink_t *s = v->sink;
    if (size > s->flash->slot_size || offset > size) {
        return PROTO_BAD_OFFSET;
    }
    if (!claim(v)) {
        return PROTO_BUSY;
    }
    s->open = false;
    proto_status_t st = from_flash(s->flash->resume(s->flash->ctx, offset));
    if (st == PROTO_OK) {
        s->open = true;
        s->written = offset;
    } else {
        release(v);
    }
    return st;
}

static proto_status_t sink_write(void *ctx, uint32_t offset, const void *data, size_t len)
{
    const ota_view_t *v = ctx;
    ota_sink_t *s = v->sink;
    if (!owns(v)) {
        return not_owner(v);
    }
    if (!s->open) {
        return PROTO_NO_SESSION;
    }
    if (offset != s->written) {
        return PROTO_BAD_OFFSET;
    }
    if (len > s->flash->slot_size - offset) {
        return PROTO_NO_SPACE;
    }
    proto_status_t st = from_flash(s->flash->write(s->flash->ctx, data, len));
    if (st == PROTO_OK) {
        s->written += (uint32_t)len;
    }
    return st;
}

static proto_status_t sink_read(void *ctx, uint32_t offset, void *buf, size_t len)
{
    const ota_view_t *v = ctx;
    ota_sink_t *s = v->sink;
    if (!owns(v) || !s->open || offset > s->written || len > s->written - offset) {
        return PROTO_INTERNAL;
    }
    return s->flash->read(s->flash->ctx, offset, buf, len) == OTA_FLASH_OK ? PROTO_OK : PROTO_INTERNAL;
}

static proto_status_t sink_finish(void *ctx, uint32_t size)
{
    const ota_view_t *v = ctx;
    ota_sink_t *s = v->sink;
    if (!owns(v)) {
        return not_owner(v);
    }
    if (!s->open) {
        release(v);
        return PROTO_NO_SESSION;
    }
    if (s->written != size) {
        return PROTO_IMAGE_INVALID;
    }
    s->open = false; /* the flash gives the handle up whatever the verdict is */
    ota_end_t e = s->flash->end(s->flash->ctx, size);
    release(v);
    switch (e) {
    case OTA_END_OK:
        return PROTO_OK;
    case OTA_END_BAD_IMAGE:
        return PROTO_IMAGE_INVALID;
    case OTA_END_BAD_SIGNATURE:
        return PROTO_SIGNATURE_INVALID;
    default:
        return PROTO_INTERNAL;
    }
}

/* Abort only ends the caller's own image: a transfer that is cleaning up must not kill the other path's download. */
static void sink_abort(void *ctx)
{
    const ota_view_t *v = ctx;
    ota_sink_t *s = v->sink;
    if (atomic_load(&s->owner) == OTA_OWNER_NONE) {
        s->flash->abort(s->flash->ctx); /* a handle left open by a session whose link dropped */
    } else if (!owns(v)) {
        return;
    } else {
        s->flash->abort(s->flash->ctx);
    }
    s->open = false;
    s->written = 0;
    release(v);
}

static xfer_fw_sink_t make_iface(const ota_flash_t *flash, ota_view_t *v)
{
    return (xfer_fw_sink_t){.slot_size = flash->slot_size, .begin = sink_begin, .resume = sink_resume,
                            .write = sink_write, .read = sink_read, .finish = sink_finish,
                            .abort = sink_abort, .ctx = v};
}

void ota_sink_init(ota_sink_t *s, const ota_flash_t *flash)
{
    s->flash = flash;
    s->written = 0;
    s->open = false;
    atomic_init(&s->owner, OTA_OWNER_NONE);
    s->views[0] = (ota_view_t){s, OTA_OWNER_BLE};
    s->views[1] = (ota_view_t){s, OTA_OWNER_WIFI};
    s->iface = make_iface(flash, &s->views[0]);
    s->wifi = make_iface(flash, &s->views[1]);
}

void ota_sink_ble_gone(ota_sink_t *s) { release(&s->views[0]); }

bool ota_sink_busy(const ota_sink_t *s) { return atomic_load(&s->owner) != OTA_OWNER_NONE; }
