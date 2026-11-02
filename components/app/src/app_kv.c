/* kv_storage_t on one NVS namespace (PLAN D26: one record per key, replaced atomically by NVS). */
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "app_priv.h"

#define TAG "kv"

static kv_status_t get(void *ctx, const char *key, void *buf, size_t cap, size_t *len)
{
    kv_nvs_t *k = ctx;
    size_t n = 0;
    esp_err_t err = nvs_get_blob(k->h, key, NULL, &n);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return KV_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return KV_ERR;
    }
    if (n > cap) {
        *len = n;
        return KV_TOO_SMALL;
    }
    err = nvs_get_blob(k->h, key, buf, &n);
    if (err != ESP_OK) {
        return KV_ERR;
    }
    *len = n;
    return KV_OK;
}

static kv_status_t set(void *ctx, const char *key, const void *data, size_t len)
{
    kv_nvs_t *k = ctx;
    if (nvs_set_blob(k->h, key, data, len) != ESP_OK || nvs_commit(k->h) != ESP_OK) {
        return KV_ERR;
    }
    return KV_OK;
}

static kv_status_t erase(void *ctx, const char *key)
{
    kv_nvs_t *k = ctx;
    const esp_err_t err = nvs_erase_key(k->h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return KV_OK;
    }
    return err == ESP_OK && nvs_commit(k->h) == ESP_OK ? KV_OK : KV_ERR;
}

bool app_kv_open(kv_nvs_t *k, const char *ns)
{
    const esp_err_t err = nvs_open(ns, NVS_READWRITE, &k->h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s): %s", ns, esp_err_to_name(err));
        return false;
    }
    k->iface = (kv_storage_t){.get = get, .set = set, .erase = erase, .ctx = k};
    return true;
}
