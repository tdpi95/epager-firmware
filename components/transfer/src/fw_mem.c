/* In-memory firmware sink for host tests and the fake board. */
#include <string.h>

#include "epager/xfer.h"

static proto_status_t fw_begin(void *ctx, uint32_t size)
{
    fw_mem_t *m = ctx;
    (void)size;
    if (m->begin_status != PROTO_OK) {
        return m->begin_status;
    }
    m->written = 0;
    m->boot_set = false;
    return PROTO_OK;
}

static proto_status_t fw_resume(void *ctx, uint32_t size, uint32_t offset)
{
    fw_mem_t *m = ctx;
    (void)size;
    return offset <= m->written ? PROTO_OK : PROTO_INTERNAL;
}

static proto_status_t fw_write(void *ctx, uint32_t offset, const void *data, size_t len)
{
    fw_mem_t *m = ctx;
    if (offset > m->iface.slot_size || len > m->iface.slot_size - offset) {
        return PROTO_NO_SPACE;
    }
    if (m->power) {
        if (*m->power <= 0) {
            return PROTO_INTERNAL;
        }
        (*m->power)--;
    }
    memcpy(m->buf + offset, data, len);
    if (offset + len > m->written) {
        m->written = offset + (uint32_t)len;
    }
    return PROTO_OK;
}

static proto_status_t fw_read(void *ctx, uint32_t offset, void *buf, size_t len)
{
    fw_mem_t *m = ctx;
    if (offset > m->iface.slot_size || len > m->iface.slot_size - offset) {
        return PROTO_INTERNAL;
    }
    memcpy(buf, m->buf + offset, len);
    return PROTO_OK;
}

static proto_status_t fw_finish(void *ctx, uint32_t size)
{
    fw_mem_t *m = ctx;
    (void)size;
    m->boot_set = m->image_status == PROTO_OK;
    return m->image_status;
}

static void fw_abort(void *ctx)
{
    fw_mem_t *m = ctx;
    m->written = 0;
}

void fw_mem_init(fw_mem_t *m, uint8_t *buf, uint32_t slot_size)
{
    memset(m, 0, sizeof(*m));
    m->buf = buf;
    m->image_status = PROTO_OK;
    m->iface = (xfer_fw_sink_t){.slot_size = slot_size, .begin = fw_begin, .resume = fw_resume, .write = fw_write,
                                .read = fw_read, .finish = fw_finish, .abort = fw_abort, .ctx = m};
}
