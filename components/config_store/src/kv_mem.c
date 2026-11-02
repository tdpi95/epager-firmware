#include <string.h>

#include "epager/kv_storage.h"

static kv_mem_entry_t *find(kv_mem_t *m, const char *key)
{
    for (size_t i = 0; i < KV_MEM_MAX_ENTRIES; i++) {
        if (m->e[i].used && strcmp(m->e[i].key, key) == 0) {
            return &m->e[i];
        }
    }
    return NULL;
}

static bool power_ok(kv_mem_t *m)
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

static kv_status_t mem_get(void *ctx, const char *key, void *buf, size_t cap, size_t *len)
{
    kv_mem_entry_t *e = find(ctx, key);
    if (!e) {
        return KV_NOT_FOUND;
    }
    if (e->len > cap) {
        return KV_TOO_SMALL;
    }
    memcpy(buf, e->val, e->len);
    *len = e->len;
    return KV_OK;
}

static kv_status_t mem_set(void *ctx, const char *key, const void *data, size_t len)
{
    kv_mem_t *m = ctx;
    if (strlen(key) > KV_KEY_MAX || len > KV_MEM_MAX_VALUE) {
        return KV_ERR;
    }
    kv_mem_entry_t *e = find(m, key);
    for (size_t i = 0; !e && i < KV_MEM_MAX_ENTRIES; i++) {
        if (!m->e[i].used) {
            e = &m->e[i];
        }
    }
    if (!e || !power_ok(m)) {
        return KV_ERR;
    }
    e->used = true;
    strcpy(e->key, key);
    memcpy(e->val, data, len);
    e->len = (uint16_t)len;
    m->mutations++;
    return KV_OK;
}

static kv_status_t mem_erase(void *ctx, const char *key)
{
    kv_mem_t *m = ctx;
    kv_mem_entry_t *e = find(m, key);
    if (!e) {
        return KV_OK;
    }
    if (!power_ok(m)) {
        return KV_ERR;
    }
    e->used = false;
    m->mutations++;
    return KV_OK;
}

void kv_mem_init(kv_mem_t *m)
{
    memset(m, 0, sizeof(*m));
    m->iface = (kv_storage_t){.get = mem_get, .set = mem_set, .erase = mem_erase, .ctx = m};
}

size_t kv_mem_count(const kv_mem_t *m)
{
    size_t n = 0;
    for (size_t i = 0; i < KV_MEM_MAX_ENTRIES; i++) {
        n += m->e[i].used;
    }
    return n;
}
