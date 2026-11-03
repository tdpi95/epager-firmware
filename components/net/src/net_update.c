/*
 * Firmware updates over Wi-Fi (docs/wifi-ota-plan.md), on the Wi-Fi task with the radio up: read the release
 * manifest, decide with upd_decide, and on request stream the image into the firmware sink with upd_download.
 * Redirects are followed by hand so that every hop can be checked against upd_url_allowed. The URLs hold no
 * personal data; the log says the step and the status only.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "epager/net_update.h"
#include "epager/upd.h"
#include "net_priv.h"
#include "sdkconfig.h"

#define MANIFEST_MAX 4096u
#define BLOCK 4096u
#define URL_MAX 1280u          /* the redirect to the release host is about 1000 characters */
#define HOPS_MAX 5
#define HTTP_TIMEOUT_MS 20000
#define MIN_VALID_UTC 1767225600u /* 2026-01-01: a clock earlier than this was never set */

static net_update_cfg_t cfg;
static char manifest_url[URL_MAX] = CONFIG_EPAGER_UPDATE_URL;
static volatile bool cancel_requested;
static EXT_RAM_BSS_ATTR char manifest[MANIFEST_MAX];
static EXT_RAM_BSS_ATTR uint8_t block[BLOCK];
static char cur_url[URL_MAX], location[URL_MAX], content_range[64];
static char image_url[URL_MAX];

typedef struct {
    esp_http_client_handle_t h;
} http_t;

/* ---------------------------------------------------------------- status */

static void set_state(net_upd_state_t state, net_upd_error_t err)
{
    net_lock();
    net.st.upd_state = (uint8_t)state;
    net.st.upd_error = (uint8_t)err;
    if (state != NET_UPD_DOWNLOADING) {
        net.st.upd_pct = 0;
    }
    net_unlock();
}

/* A step that never got to the release host. Also ends a check the policy is waiting for (check_ok false). */
void net_update_fail(uint8_t error)
{
    net_lock();
    net.st.upd_state = NET_UPD_FAILED;
    net.st.upd_error = error;
    net.st.upd_pct = 0;
    net.st.upd_check_ok = false;
    net.st.upd_check_seq++;
    net_unlock();
}

static void publish_check(bool ok, upd_verdict_t verdict, const upd_image_t *img)
{
    net_lock();
    net.st.upd_check_ok = ok;
    net.st.upd_check_seq++;
    if (ok) {
        net.st.upd_verdict = (uint8_t)verdict;
        snprintf(net.st.upd_version, sizeof(net.st.upd_version), "%s", img->version);
        net.st.upd_size = img->size;
    }
    net.st.upd_state = ok ? NET_UPD_IDLE : NET_UPD_FAILED;
    net.st.upd_error = ok ? NET_UPD_ERR_NONE : NET_UPD_ERR_NET;
    net_unlock();
}

static void publish_pct(uint8_t pct)
{
    if (pct != net.st.upd_pct) {
        net_lock();
        net.st.upd_pct = pct;
        net_unlock();
    }
}

bool net_update_clock_ok(void) { return (uint32_t)time(NULL) >= MIN_VALID_UTC; }

void net_update_start(const net_update_cfg_t *c) { cfg = *c; }

void net_update_set_url_internal(const char *url)
{
    if (url != NULL && strlen(url) < sizeof(manifest_url) && upd_url_allowed(url)) {
        snprintf(manifest_url, sizeof(manifest_url), "%s", url);
    }
}

void net_update_cancel_internal(void) { cancel_requested = true; }

/* ---------------------------------------------------------------- HTTP */

static esp_err_t on_event(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_HEADER || e->header_key == NULL || e->header_value == NULL) {
        return ESP_OK;
    }
    if (strcasecmp(e->header_key, "Location") == 0) {
        /* a value that does not fit stays empty: the redirect is then refused */
        if (strlen(e->header_value) < sizeof(location)) {
            snprintf(location, sizeof(location), "%s", e->header_value);
        }
    } else if (strcasecmp(e->header_key, "Content-Range") == 0 && strlen(e->header_value) < sizeof(content_range)) {
        snprintf(content_range, sizeof(content_range), "%s", e->header_value);
    }
    return ESP_OK;
}

/* "bytes FIRST-LAST/TOTAL" */
static void parse_content_range(upd_http_head_t *head)
{
    unsigned long first, last, total;
    head->range_first = 0;
    head->range_total = UINT32_MAX;
    if (sscanf(content_range, "bytes %lu-%lu/%lu", &first, &last, &total) == 3 && last >= first && total <= UINT32_MAX) {
        head->range_first = (uint32_t)first;
        head->range_total = (uint32_t)total;
    }
}

/* GET start_url (from byte `offset` when > 0) and follow the redirects by hand. 0: the final answer's headers are
 * in head and the body can be read; the connection is open. */
static int http_start(http_t *t, const char *start_url, uint32_t offset, upd_http_head_t *head)
{
    snprintf(cur_url, sizeof(cur_url), "%s", start_url);
    for (int hop = 0; hop < HOPS_MAX; hop++) {
        location[0] = content_range[0] = '\0';
        if (esp_http_client_set_url(t->h, cur_url) != ESP_OK) {
            return -1;
        }
        if (offset > 0) {
            char range[32];
            snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)offset);
            esp_http_client_set_header(t->h, "Range", range);
        } else {
            esp_http_client_delete_header(t->h, "Range");
        }
        if (esp_http_client_open(t->h, 0) != ESP_OK) {
            return -1;
        }
        const int64_t len = esp_http_client_fetch_headers(t->h);
        if (len < 0) {
            esp_http_client_close(t->h);
            return -1;
        }
        const int status = esp_http_client_get_status_code(t->h);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_close(t->h);
            if (!upd_url_allowed(location)) {
                ESP_LOGW(NET_TAG, "update: redirect refused");
                return -1;
            }
            snprintf(cur_url, sizeof(cur_url), "%s", location);
            continue;
        }
        head->status = status;
        head->content_len = len > 0 ? (uint32_t)len : UINT32_MAX;
        parse_content_range(head);
        return 0;
    }
    ESP_LOGW(NET_TAG, "update: too many redirects");
    return -1;
}

static bool http_init(http_t *t)
{
    /* The signed release URL is about 1000 characters: it must fit the request line (tx) and the Location header (rx). */
    esp_http_client_config_t c = {.url = manifest_url, .crt_bundle_attach = esp_crt_bundle_attach,
                                  .timeout_ms = HTTP_TIMEOUT_MS, .user_agent = cfg.user_agent,
                                  .buffer_size = 2048, .buffer_size_tx = 2048, .disable_auto_redirect = true,
                                  .event_handler = on_event};
    t->h = esp_http_client_init(&c);
    return t->h != NULL;
}

static void http_done(http_t *t)
{
    if (t->h != NULL) {
        esp_http_client_close(t->h);
        esp_http_client_cleanup(t->h);
        t->h = NULL;
    }
}

static int tr_open(void *ctx, uint32_t offset, upd_http_head_t *head)
{
    return http_start(ctx, image_url, offset, head);
}

static int64_t read_us; /* time spent waiting for the network in this download: the rest is flash and hashing */
static volatile uint32_t drop_after; /* test hook (console `update drop N`): the next download fails once after N bytes */
static uint32_t body_bytes;

void net_update_debug_drop(uint32_t after_bytes) { drop_after = after_bytes; }

static int tr_read(void *ctx, void *buf, size_t cap)
{
    if (drop_after != 0 && body_bytes >= drop_after) {
        drop_after = 0;
        ESP_LOGW(NET_TAG, "update: dropping the connection after %u bytes (test)", (unsigned)body_bytes);
        return -1;
    }
    const int64_t t0 = esp_timer_get_time();
    const int n = esp_http_client_read(((http_t *)ctx)->h, buf, (int)cap);
    read_us += esp_timer_get_time() - t0;
    if (n > 0) {
        body_bytes += (uint32_t)n;
    }
    return n;
}

static void tr_close(void *ctx) { esp_http_client_close(((http_t *)ctx)->h); }

/* ---------------------------------------------------------------- manifest */

static bool fetch_manifest(upd_verdict_t *verdict, upd_image_t *img)
{
    http_t t;
    if (!http_init(&t)) {
        return false;
    }
    upd_http_head_t head = {0};
    bool ok = false;
    if (http_start(&t, manifest_url, 0, &head) == 0 && head.status == 200) {
        size_t len = 0;
        int n;
        while (len < sizeof(manifest) && (n = esp_http_client_read(t.h, manifest + len, (int)(sizeof(manifest) - len))) > 0) {
            len += (size_t)n;
        }
        char more;
        ok = n >= 0 && !(len == sizeof(manifest) && esp_http_client_read(t.h, &more, 1) > 0); /* not larger than the buffer */
        if (ok) {
            upd_board_t b = {.board_id = cfg.board_id, .variant = cfg.variant, .running_version = cfg.running_version,
                             .contract_version = cfg.contract_version, .slot_size = cfg.slot_size,
                             .allow_prerelease = strstr(manifest_url, "/releases/latest/") == NULL};
            *verdict = upd_decide(manifest, len, &b, img);
            ok = *verdict != UPD_INVALID; /* a cut-off or odd answer is a failed check, tried again later */
        }
    }
    ESP_LOGI(NET_TAG, "update: manifest status %d, %s", head.status, ok ? "read" : "not usable");
    http_done(&t);
    return ok;
}

void net_update_run_check(void)
{
    cancel_requested = false;
    set_state(NET_UPD_CHECKING, NET_UPD_ERR_NONE);
    upd_verdict_t verdict = UPD_INVALID;
    upd_image_t img;
    memset(&img, 0, sizeof(img));
    const bool ok = fetch_manifest(&verdict, &img);
    ESP_LOGI(NET_TAG, "update: check %s, verdict %d", ok ? "done" : "failed", (int)verdict);
    publish_check(ok, verdict, &img);
}

/* ---------------------------------------------------------------- install */

static void on_progress(void *ctx, uint32_t done, uint32_t total)
{
    (void)ctx;
    publish_pct((uint8_t)((uint64_t)done * 100u / total));
}

static bool on_cancelled(void *ctx)
{
    (void)ctx;
    return cancel_requested;
}

static net_upd_error_t error_for(upd_dl_result_t r, proto_status_t sink_status)
{
    switch (r) {
    case UPD_DL_NETWORK:
        return NET_UPD_ERR_NET;
    case UPD_DL_BAD_SIZE:
    case UPD_DL_HASH:
        return NET_UPD_ERR_DOWNLOAD;
    case UPD_DL_CANCELLED:
        return NET_UPD_ERR_CANCELLED;
    case UPD_DL_REJECTED:
        return NET_UPD_ERR_REJECTED;
    case UPD_DL_SINK:
        return sink_status == PROTO_BUSY ? NET_UPD_ERR_BUSY : NET_UPD_ERR_INTERNAL;
    default:
        return NET_UPD_ERR_INTERNAL;
    }
}

void net_update_run_install(const char *version)
{
    cancel_requested = false;
    set_state(NET_UPD_CHECKING, NET_UPD_ERR_NONE);
    upd_verdict_t verdict = UPD_INVALID;
    upd_image_t img;
    memset(&img, 0, sizeof(img));
    if (!fetch_manifest(&verdict, &img)) {
        publish_check(false, verdict, &img);
        return;
    }
    if (verdict != UPD_AVAILABLE || strcmp(img.version, version) != 0) {
        /* the newest release is not the one the person agreed to: report it as a check, and ask again */
        ESP_LOGI(NET_TAG, "update: the newest release changed, not installing");
        publish_check(true, verdict, &img);
        return;
    }
    if (!upd_image_url(manifest_url, img.version, img.file, image_url, sizeof(image_url)) || !upd_url_allowed(image_url)) {
        set_state(NET_UPD_FAILED, NET_UPD_ERR_INTERNAL);
        return;
    }
    http_t t;
    if (!http_init(&t)) {
        set_state(NET_UPD_FAILED, NET_UPD_ERR_INTERNAL);
        return;
    }
    set_state(NET_UPD_DOWNLOADING, NET_UPD_ERR_NONE);
    upd_download_params_t p = {.sink = cfg.sink, .http = {tr_open, tr_read, tr_close, &t}, .size = img.size,
                               .max_resumes = 3, .buf = block, .buf_len = sizeof(block), .progress = on_progress,
                               .cancelled = on_cancelled};
    memcpy(p.sha256, img.sha256, sizeof(p.sha256));
    proto_status_t sink_status = PROTO_OK;
    read_us = 0;
    body_bytes = 0;
    const int64_t start_us = esp_timer_get_time();
    const upd_dl_result_t r = upd_download(&p, &sink_status);
    const int64_t total_us = esp_timer_get_time() - start_us;
    http_done(&t);
    ESP_LOGI(NET_TAG, "update: download result %d (sink status %d), %u bytes in %u ms, %u ms of it reading the network",
             (int)r, (int)sink_status, (unsigned)img.size, (unsigned)(total_us / 1000), (unsigned)(read_us / 1000));
    if (r == UPD_DL_OK) {
        set_state(NET_UPD_INSTALLED, NET_UPD_ERR_NONE);
    } else {
        set_state(NET_UPD_FAILED, error_for(r, sink_status));
    }
}
