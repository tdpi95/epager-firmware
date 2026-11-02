/* In-memory blob_storage for host tests and the fake board. Files are listed in name order. */
#include <stdlib.h>
#include <string.h>

#include "epager/blob_storage.h"

static blob_mem_file_t *find(blob_mem_t *m, const char *name)
{
    for (size_t i = 0; i < BLOB_MEM_MAX_FILES; i++) {
        if (m->f[i].used && strcmp(m->f[i].name, name) == 0) {
            return &m->f[i];
        }
    }
    return NULL;
}

static uint32_t used_bytes(const blob_mem_t *m)
{
    uint32_t n = 0;
    for (size_t i = 0; i < BLOB_MEM_MAX_FILES; i++) {
        n += m->f[i].used ? m->f[i].size : 0;
    }
    return n;
}

static bool power_ok(blob_mem_t *m)
{
    if (!m->power) {
        return true;
    }
    if (*m->power <= 0) {
        return false;
    }
    (*m->power)--;
    return true;
}

static void drop(blob_mem_file_t *f)
{
    free(f->data);
    memset(f, 0, sizeof(*f));
}

static blob_status_t mem_stat(void *ctx, const char *name, uint32_t *size)
{
    blob_mem_file_t *f = find(ctx, name);
    if (!f) {
        return BLOB_NOT_FOUND;
    }
    *size = f->size;
    return BLOB_OK;
}

static blob_status_t mem_read(void *ctx, const char *name, uint32_t off, void *buf, size_t len)
{
    blob_mem_file_t *f = find(ctx, name);
    if (!f) {
        return BLOB_NOT_FOUND;
    }
    if (off > f->size || len > f->size - off) {
        return BLOB_ERR;
    }
    if (len) {
        memcpy(buf, f->data + off, len);
    }
    return BLOB_OK;
}

static blob_status_t mem_write(void *ctx, const char *name, uint32_t off, const void *data, size_t len)
{
    blob_mem_t *m = ctx;
    if (strlen(name) > BLOB_NAME_MAX) {
        return BLOB_ERR;
    }
    blob_mem_file_t *f = find(m, name);
    uint32_t cur = f ? f->size : 0;
    if (off > cur) {
        return BLOB_ERR;
    }
    uint32_t end = off + (uint32_t)len, grow = end > cur ? end - cur : 0;
    if (grow > m->total - used_bytes(m)) {
        return BLOB_NO_SPACE;
    }
    for (size_t i = 0; !f && i < BLOB_MEM_MAX_FILES; i++) {
        if (!m->f[i].used) {
            f = &m->f[i];
        }
    }
    if (!f || !power_ok(m)) {
        return BLOB_ERR;
    }
    if (grow) {
        uint8_t *d = realloc(f->data, end);
        if (!d) {
            return BLOB_ERR;
        }
        f->data = d;
        f->size = end;
    }
    if (!f->used) {
        f->used = true;
        strcpy(f->name, name);
    }
    if (len) {
        memcpy(f->data + off, data, len);
    }
    return BLOB_OK;
}

static blob_status_t mem_flush(void *ctx, const char *name)
{
    return find(ctx, name) ? BLOB_OK : BLOB_NOT_FOUND;
}

static blob_status_t mem_rename(void *ctx, const char *from, const char *to)
{
    blob_mem_t *m = ctx;
    blob_mem_file_t *f = find(m, from), *t = find(m, to);
    if (!f) {
        return BLOB_NOT_FOUND;
    }
    if (strlen(to) > BLOB_NAME_MAX || !power_ok(m)) {
        return BLOB_ERR;
    }
    if (t && t != f) {
        drop(t);
    }
    strcpy(f->name, to);
    return BLOB_OK;
}

static blob_status_t mem_remove(void *ctx, const char *name)
{
    blob_mem_t *m = ctx;
    blob_mem_file_t *f = find(m, name);
    if (!f) {
        return BLOB_OK;
    }
    if (!power_ok(m)) {
        return BLOB_ERR;
    }
    drop(f);
    return BLOB_OK;
}

static blob_status_t mem_list(void *ctx, uint32_t index, char *name, size_t cap, uint32_t *size)
{
    blob_mem_t *m = ctx;
    const blob_mem_file_t *prev = NULL;
    for (uint32_t k = 0; k <= index; k++) { /* k-th smallest name */
        const blob_mem_file_t *best = NULL;
        for (size_t i = 0; i < BLOB_MEM_MAX_FILES; i++) {
            const blob_mem_file_t *f = &m->f[i];
            if (f->used && (!prev || strcmp(f->name, prev->name) > 0) && (!best || strcmp(f->name, best->name) < 0)) {
                best = f;
            }
        }
        if (!best) {
            return BLOB_NOT_FOUND;
        }
        prev = best;
    }
    if (strlen(prev->name) >= cap) {
        return BLOB_ERR;
    }
    strcpy(name, prev->name);
    *size = prev->size;
    return BLOB_OK;
}

static blob_status_t mem_usage(void *ctx, uint32_t *total, uint32_t *free_bytes)
{
    blob_mem_t *m = ctx;
    *total = m->total;
    *free_bytes = m->total - used_bytes(m);
    return BLOB_OK;
}

void blob_mem_init(blob_mem_t *m, uint32_t total)
{
    memset(m, 0, sizeof(*m));
    m->total = total;
    m->iface = (blob_storage_t){.stat = mem_stat, .read = mem_read, .write = mem_write, .flush = mem_flush,
                                .rename = mem_rename, .remove = mem_remove, .list = mem_list, .usage = mem_usage,
                                .ctx = m};
}

void blob_mem_free(blob_mem_t *m)
{
    for (size_t i = 0; i < BLOB_MEM_MAX_FILES; i++) {
        if (m->f[i].used) {
            drop(&m->f[i]);
        }
    }
}

const uint8_t *blob_mem_data(const blob_mem_t *m, const char *name, uint32_t *size)
{
    blob_mem_file_t *f = find((blob_mem_t *)m, name);
    if (!f) {
        return NULL;
    }
    *size = f->size;
    return f->data ? f->data : (const uint8_t *)"";
}
