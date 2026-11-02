/*
 * The Wi-Fi task (PLAN D60-D63). Joins the stored network only for a fetch cycle (components/weather
 * decides when and which requests), takes the time from SNTP at most once a day (when SNTP does not
 * answer, e.g. UDP 123 blocked on an office network, from the Date header of the HTTPS replies), then
 * switches the radio off. Credentials live in NVS through esp_wifi (unencrypted: NVS encryption needs eFuses, D61). Logs
 * carry the request kind and status only: the URL holds the place, which is personal data.
 */
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#include "epager/net_util.h"
#include "epager/power.h"
#include "epager/proto_gen.h"
#include "net_priv.h"

#define TASK_STACK 8192          /* TLS handshakes run on this task */
#define TASK_PRIO 3              /* below the UI loop */
#define CMD_QUEUE 4
#define HTTP_TIMEOUT_MS 10000
#define SNTP_WAIT_MS 5000
#define SNTP_EVERY_MS (24u * 3600u * 1000u)
#define BODY_MAX 4096            /* the largest answer: geocoding with WX_GEOCODE_COUNT places, about 1.7 KB */
#define NET_WIFI_PROFILE_MAX 3u
#define NET_SCAN_MAX 16u
#define PROFILE_MAGIC 0x45535733u /* "ESW3" */
#define PROFILE_KEY "wifi.profiles"

typedef enum { CMD_CONFIG, CMD_WIFI_SET, CMD_SETUP, CMD_SAVE, CMD_KICK } cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    bool flag;                   /* CONFIG: enabled; WIFI_SET: forget; SETUP: open */
    uint16_t interval_min;
    uint8_t language;
    char ssid[33], pw[64], city[WX_CITY_MAX + 1];
    size_t city_len;
} cmd_t;

net_shared_t net;

static QueueHandle_t cmds;
static wx_t wx;
static char user_agent[48];
static EXT_RAM_BSS_ATTR char body[BODY_MAX]; /* PSRAM where the image allows it */
static char url[WX_URL_MAX];
static bool sntp_done;
static uint32_t sntp_last_ms;
static bool date_seen;               /* this cycle's newest Date header: */
static uint32_t date_utc, date_ms;   /* UTC seconds at monotonic milliseconds date_ms */
static const char *const REQ_NAME[] = {"none", "geocode", "ipinfo", "forecast"};

typedef struct {
    char ssid[33], pw[64];
    uint32_t used;                /* monotonically increasing recency, 0 = unused */
} net_profile_t;

typedef struct {
    uint32_t magic, next_used;
    net_profile_t entry[NET_WIFI_PROFILE_MAX];
} net_profiles_t;

static net_profiles_t profiles;
static wifi_ap_record_t scanned[NET_SCAN_MAX];

/* ---------------------------------------------------------------- shared status */

void net_lock(void) { xSemaphoreTake(net.lock, portMAX_DELAY); }

void net_unlock(void)
{
    net.gen++;
    xSemaphoreGive(net.lock);
}

bool net_poll(net_status_t *out)
{
    static uint32_t seen;
    if (net.lock == NULL) {
        return false;
    }
    xSemaphoreTake(net.lock, portMAX_DELAY);
    const bool changed = net.gen != seen;
    if (changed) {
        *out = net.st;
        seen = net.gen;
    }
    xSemaphoreGive(net.lock);
    return changed;
}

static void set_wifi_state(uint8_t state)
{
    if (net.st.wifi_state != state) {
        net_lock();
        net.st.wifi_state = state;
        net_unlock();
    }
}

/* The weather state and the reading to show, when they changed (staleness included). */
static void publish_weather(void)
{
    const uint32_t now = net.now_ms();
    const wx_reading_t *r = wx_current(&wx, now);
    const uint8_t state = wx_state(&wx, now);
    if (state == net.st.weather_state && (r != NULL) == net.st.have_reading &&
        (r == NULL || memcmp(r, &net.st.reading, sizeof(*r)) == 0)) {
        return;
    }
    net_lock();
    net.st.weather_state = state;
    net.st.have_reading = r != NULL;
    if (r != NULL) {
        net.st.reading = *r;
    }
    net_unlock();
}

/* ---------------------------------------------------------------- station */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        net.disc_reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
        xEventGroupSetBits(net.ev, NET_BIT_FAIL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(net.ev, NET_BIT_ASSOC);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(net.ev, NET_BIT_GOT_IP);
    }
}

static uint8_t state_for_reason(uint16_t reason, bool associated)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return PROTO_WIFI_STATE_AUTH_FAILED;
    default:
        /* associated but no address: the network is there, the way to the internet is not */
        return associated ? PROTO_WIFI_STATE_NO_INTERNET : PROTO_WIFI_STATE_NOT_FOUND;
    }
}

static bool profiles_save(void)
{
    nvs_handle_t h;
    if (nvs_open("epager", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    const esp_err_t err = nvs_set_blob(h, PROFILE_KEY, &profiles, sizeof(profiles));
    const esp_err_t committed = err == ESP_OK ? nvs_commit(h) : err;
    nvs_close(h);
    return committed == ESP_OK;
}

static bool profiles_load(void)
{
    nvs_handle_t h;
    size_t n = sizeof(profiles);
    if (nvs_open("epager", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    const esp_err_t err = nvs_get_blob(h, PROFILE_KEY, &profiles, &n);
    nvs_close(h);
    if (err != ESP_OK || n != sizeof(profiles) || profiles.magic != PROFILE_MAGIC) {
        memset(&profiles, 0, sizeof(profiles));
        profiles.magic = PROFILE_MAGIC;
        profiles.next_used = 1;
        return err == ESP_ERR_NVS_NOT_FOUND || profiles_save();
    }
    if (profiles.next_used == 0) {
        profiles.next_used = 1;
    }
    return true;
}

static int profile_find(const char *ssid)
{
    for (size_t i = 0; i < NET_WIFI_PROFILE_MAX; i++) {
        if (profiles.entry[i].ssid[0] && strcmp(profiles.entry[i].ssid, ssid) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool profiles_any(void)
{
    for (size_t i = 0; i < NET_WIFI_PROFILE_MAX; i++) {
        if (profiles.entry[i].ssid[0]) {
            return true;
        }
    }
    return false;
}

static void profile_touch(size_t i)
{
    if (profiles.next_used == 0) { /* Avoid ambiguous LRU ordering after wrapping. */
        for (size_t k = 0; k < NET_WIFI_PROFILE_MAX; k++) {
            profiles.entry[k].used = profiles.entry[k].ssid[0] ? (uint32_t)(k + 1) : 0;
        }
        profiles.next_used = NET_WIFI_PROFILE_MAX + 1;
    }
    profiles.entry[i].used = profiles.next_used++;
}

/* The access point the last join ended on, kept in RAM only: the next fetch connects to it without scanning
 * (two scans per fetch cost seconds of radio time). Any failure, or a change of the saved networks, drops it. */
typedef struct {
    bool valid;
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
} last_ap_t;

static last_ap_t last_ap;

static bool net_apply_profile(const net_profile_t *p, const last_ap_t *hint)
{
    wifi_config_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.sta.ssid, p->ssid, strnlen(p->ssid, sizeof(c.sta.ssid)));
    memcpy(c.sta.password, p->pw, strnlen(p->pw, sizeof(c.sta.password) - 1));
    c.sta.threshold.authmode = p->pw[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (hint != NULL) {
        c.sta.scan_method = WIFI_FAST_SCAN;
        c.sta.bssid_set = true;
        memcpy(c.sta.bssid, hint->bssid, sizeof(c.sta.bssid));
        c.sta.channel = hint->channel;
    }
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &c);
    memset(&c, 0, sizeof(c));
    return err == ESP_OK;
}

static bool net_join_one(size_t profile, const last_ap_t *hint, uint32_t timeout_ms, uint8_t *wifi_state)
{
    const net_profile_t *p = &profiles.entry[profile];
    if (!net_apply_profile(p, hint)) {
        *wifi_state = PROTO_WIFI_STATE_NOT_FOUND;
        return false;
    }
    net_lock();
    snprintf(net.st.ssid, sizeof(net.st.ssid), "%s", p->ssid);
    net_unlock();
    xEventGroupClearBits(net.ev, NET_BIT_GOT_IP | NET_BIT_FAIL | NET_BIT_ASSOC);
    net.disc_reason = 0;
    if (esp_wifi_connect() != ESP_OK) {
        *wifi_state = PROTO_WIFI_STATE_NOT_FOUND;
        return false;
    }
    const EventBits_t b = xEventGroupWaitBits(net.ev, NET_BIT_GOT_IP | NET_BIT_FAIL, pdFALSE, pdFALSE,
                                              pdMS_TO_TICKS(timeout_ms));
    if (b & NET_BIT_GOT_IP) {
        *wifi_state = PROTO_WIFI_STATE_CONNECTED;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            last_ap.valid = true;
            snprintf(last_ap.ssid, sizeof(last_ap.ssid), "%s", p->ssid);
            memcpy(last_ap.bssid, ap.bssid, sizeof(last_ap.bssid));
            last_ap.channel = ap.primary;
        }
        profile_touch(profile);
        (void)profiles_save();
        return true;
    }
    *wifi_state = state_for_reason(net.disc_reason, (xEventGroupGetBits(net.ev) & NET_BIT_ASSOC) != 0);
    last_ap.valid = false;
    esp_wifi_disconnect();
    return false;
}

/* The saved SSIDs for net_status_t (net lock held, or before the task starts). */
static void publish_networks(void)
{
    net.st.n_networks = 0;
    memset(net.st.networks, 0, sizeof(net.st.networks));
    for (size_t i = 0; i < NET_WIFI_PROFILE_MAX; i++) {
        if (profiles.entry[i].ssid[0]) {
            snprintf(net.st.networks[net.st.n_networks++], sizeof(net.st.networks[0]), "%s", profiles.entry[i].ssid);
        }
    }
}

bool net_store_network(const char *ssid, const char *pw)
{
    int i = ssid ? profile_find(ssid) : -1;
    if (ssid != NULL && i < 0) {
        uint32_t oldest = UINT32_MAX;
        for (size_t k = 0; k < NET_WIFI_PROFILE_MAX; k++) {
            if (!profiles.entry[k].ssid[0]) {
                i = (int)k;
                break;
            }
            if (profiles.entry[k].used < oldest) {
                oldest = profiles.entry[k].used;
                i = (int)k;
            }
        }
    }
    last_ap.valid = false; /* the password or the list changed */
    if (ssid != NULL) {
        net_profile_t *p = &profiles.entry[i];
        memset(p, 0, sizeof(*p));
        snprintf(p->ssid, sizeof(p->ssid), "%s", ssid);
        snprintf(p->pw, sizeof(p->pw), "%s", pw);
        profile_touch((size_t)i);
    } else {
        memset(profiles.entry, 0, sizeof(profiles.entry));
    }
    if (!profiles_save()) {
        ESP_LOGE(NET_TAG, "storing Wi-Fi profiles failed");
        return false;
    }
    net_lock();
    publish_networks();
    net.st.wifi_state = ssid ? PROTO_WIFI_STATE_IDLE : PROTO_WIFI_STATE_UNCONFIGURED;
    if (ssid == NULL) {
        net.st.ssid[0] = '\0';
    }
    net_unlock();
    if (ssid) {
        ESP_LOGI(NET_TAG, "network saved");
    } else {
        ESP_LOGI(NET_TAG, "networks forgotten");
    }
    return true;
}

static bool net_forget_network(const char *ssid)
{
    const int i = profile_find(ssid);
    last_ap.valid = false;
    if (i >= 0) {
        memset(&profiles.entry[i], 0, sizeof(profiles.entry[i]));
        if (!profiles_save()) {
            ESP_LOGE(NET_TAG, "forgetting Wi-Fi profile failed");
            return false;
        }
    }
    net_lock();
    publish_networks();
    if (strcmp(net.st.ssid, ssid) == 0) {
        net.st.ssid[0] = '\0';
    }
    net.st.wifi_state = profiles_any() ? PROTO_WIFI_STATE_IDLE : PROTO_WIFI_STATE_UNCONFIGURED;
    net_unlock();
    ESP_LOGI(NET_TAG, "network forgotten");
    return true;
}

static bool net_scan_candidates(size_t *candidate, size_t *n_candidate)
{
    uint16_t n = NET_SCAN_MAX;
    *n_candidate = 0;
    const wifi_scan_config_t c = {.show_hidden = false};
    if (esp_wifi_scan_start(&c, true) != ESP_OK || esp_wifi_scan_get_ap_records(&n, scanned) != ESP_OK) {
        return false;
    }
    for (uint16_t s = 0; s < n; s++) { /* ESP-IDF returns strongest first. */
        const int i = profile_find((const char *)scanned[s].ssid);
        bool duplicate = false;
        for (size_t k = 0; k < *n_candidate; k++) {
            duplicate |= candidate[k] == (size_t)i;
        }
        if (i >= 0 && !duplicate) {
            candidate[(*n_candidate)++] = (size_t)i;
        }
    }
    return true;
}

bool net_join(uint32_t timeout_ms, uint8_t *wifi_state)
{
    size_t candidate[NET_WIFI_PROFILE_MAX], n = 0;
    if (last_ap.valid) { /* TODO(hw-verify): join time and current with the remembered access point (checklist 78) */
        const last_ap_t hint = last_ap;
        const int i = profile_find(hint.ssid);
        if (i >= 0 && net_join_one((size_t)i, &hint, timeout_ms, wifi_state)) {
            return true;
        }
        last_ap.valid = false; /* moved, switched off or out of range: scan as before */
    }
    if (!net_scan_candidates(candidate, &n) || n == 0) {
        *wifi_state = PROTO_WIFI_STATE_NOT_FOUND;
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (net_join_one(candidate[i], NULL, timeout_ms, wifi_state)) {
            return true;
        }
    }
    return false;
}

bool net_join_named(const char *ssid, uint32_t timeout_ms, uint8_t *wifi_state)
{
    const int i = profile_find(ssid);
    if (i < 0) {
        *wifi_state = PROTO_WIFI_STATE_NOT_FOUND;
        return false;
    }
    return net_join_one((size_t)i, NULL, timeout_ms, wifi_state);
}

void net_weather_kick(void) { wx_kick(&wx, net.now_ms()); }

/* ---------------------------------------------------------------- HTTPS and SNTP */

/* Keeps the reply's Date header (net_util parses it; anything else is ignored). */
static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    uint32_t utc;
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
        strcasecmp(e->header_key, "Date") == 0 &&
        net_http_date(e->header_value, strlen(e->header_value), &utc)) {
        date_seen = true;
        date_utc = utc;
        date_ms = net.now_ms();
    }
    return ESP_OK;
}

/* GET url into body (NUL-terminated): the HTTP status, or -1 (no answer, or larger than the buffer). */
static int http_get(const char *u, size_t *len)
{
    esp_http_client_config_t cfg = {.url = u, .crt_bundle_attach = esp_crt_bundle_attach,
                                    .timeout_ms = HTTP_TIMEOUT_MS, .user_agent = user_agent, .buffer_size = 1024,
                                    .event_handler = on_http_event};
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    *len = 0;
    if (h == NULL) {
        return -1;
    }
    int status = -1;
    if (esp_http_client_open(h, 0) == ESP_OK && esp_http_client_fetch_headers(h) >= 0) {
        status = esp_http_client_get_status_code(h);
        const int n = esp_http_client_read_response(h, body, BODY_MAX - 1);
        char more;
        if (n < 0 || esp_http_client_read(h, &more, 1) > 0) {
            status = -1; /* a read error, or an answer that does not fit: not this service */
        } else {
            body[n] = '\0';
            *len = (size_t)n;
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return status;
}

static void publish_time(uint32_t utc, uint32_t at_ms)
{
    net_lock();
    net.st.time_seq++;
    net.st.time_utc = utc;
    net.st.time_ms = at_ms;
    net_unlock();
}

static void sntp_sync(void)
{
    const uint32_t now = net.now_ms();
    if (sntp_done && now - sntp_last_ms < SNTP_EVERY_MS) {
        return;
    }
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&c) != ESP_OK) {
        return;
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_WAIT_MS)) == ESP_OK) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        publish_time((uint32_t)tv.tv_sec, net.now_ms());
        sntp_done = true;
        sntp_last_ms = now;
        ESP_LOGI(NET_TAG, "time from SNTP");
    } else {
        ESP_LOGW(NET_TAG, "no SNTP answer");
    }
    esp_netif_sntp_deinit();
}

/* ---------------------------------------------------------------- fetch cycle */

static void fetch_cycle_locked(void);

/* A fetch cycle is short and CPU-bound (TLS): no frequency scaling, no light sleep (PLAN D17). */
static void fetch_cycle(void)
{
    power_hold(POWER_LOCK_CPU_MAX);
    power_hold(POWER_LOCK_NO_SLEEP);
    fetch_cycle_locked();
    power_release(POWER_LOCK_NO_SLEEP);
    power_release(POWER_LOCK_CPU_MAX);
}

static void fetch_cycle_locked(void)
{
    set_wifi_state(PROTO_WIFI_STATE_CONNECTING);
    uint8_t state = PROTO_WIFI_STATE_NOT_FOUND; /* if the radio does not even start */
    if (esp_wifi_start() != ESP_OK || !net_join(NET_JOIN_TIMEOUT_MS, &state)) {
        esp_wifi_stop();
        wx_no_network(&wx, net.now_ms());
        set_wifi_state(state);
        if (state == PROTO_WIFI_STATE_NO_INTERNET) {
            ESP_LOGW(NET_TAG, "join failed: associated, but no IP address within %u s", NET_JOIN_TIMEOUT_MS / 1000u);
        } else {
            ESP_LOGW(NET_TAG, "join failed (state %u, reason %u)", state, net.disc_reason);
        }
        return;
    }
    set_wifi_state(PROTO_WIFI_STATE_CONNECTED);
    sntp_sync();
    date_seen = false;
    wx_begin(&wx, net.now_ms());
    bool asked = false, answered = false;
    wx_req_t r;
    while ((r = wx_next(&wx, url, sizeof(url))) != WX_REQ_NONE) {
        size_t n;
        const int status = http_get(url, &n);
        asked = true;
        answered |= status > 0;
        ESP_LOGI(NET_TAG, "%s -> %d (%u bytes)", REQ_NAME[r], status, (unsigned)n);
        wx_response(&wx, net.now_ms(), status, body, n);
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    if (!sntp_done && date_seen) { /* no SNTP within a day: the servers' clock, to the second */
        publish_time(date_utc, date_ms);
        ESP_LOGI(NET_TAG, "time from an HTTPS Date header");
    }
    set_wifi_state(asked && !answered ? PROTO_WIFI_STATE_NO_INTERNET : PROTO_WIFI_STATE_IDLE);
}

/* ---------------------------------------------------------------- task */

static void handle(cmd_t *c)
{
    switch (c->kind) {
    case CMD_CONFIG:
        net.language = c->language;
        memcpy(net.city, c->city, c->city_len);
        net.city[c->city_len] = '\0';
        wx_configure(&wx, net.now_ms(), c->flag, c->interval_min, (const uint8_t *)c->city, c->city_len,
                     c->language == PROTO_LANGUAGE_VI ? "vi" : "en");
        break;
    case CMD_WIFI_SET:
        if ((c->flag ? (c->ssid[0] ? net_forget_network(c->ssid) : net_store_network(NULL, NULL))
                     : net_store_network(c->ssid, c->pw)) && !c->flag) {
            wx_kick(&wx, net.now_ms());
        }
        break;
    case CMD_SETUP:
        if (c->flag) {
            net_setup_start();
        } else {
            net_setup_stop();
        }
        break;
    case CMD_SAVE:
        net_setup_save(c->ssid, c->pw, c->city);
        break;
    case CMD_KICK:
        wx_kick(&wx, net.now_ms());
        break;
    }
    memset(c->pw, 0, sizeof(c->pw));
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_t c;
        if (xQueueReceive(cmds, &c, pdMS_TO_TICKS(1000)) == pdTRUE) {
            handle(&c);
        }
        if (net_setup_active()) {
            net_setup_service();
        } else if (profiles_any() && wx_due(&wx, net.now_ms())) {
            fetch_cycle();
        }
        publish_weather();
    }
}

static bool post(cmd_t *c)
{
    const bool ok = cmds != NULL && xQueueSend(cmds, c, 0) == pdTRUE;
    memset(c->pw, 0, sizeof(c->pw));
    return ok;
}

bool net_start(const char *ua, uint32_t (*now_ms)(void))
{
    net.now_ms = now_ms;
    snprintf(user_agent, sizeof(user_agent), "%s", ua);
    net.lock = xSemaphoreCreateMutex();
    net.ev = xEventGroupCreate();
    cmds = xQueueCreate(CMD_QUEUE, sizeof(cmd_t));
    if (net.lock == NULL || net.ev == NULL || cmds == NULL) {
        return false;
    }
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
        err = err == ESP_ERR_INVALID_STATE ? ESP_OK : err; /* someone created it already */
    }
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "netif / event loop: %s", esp_err_to_name(err));
        return false;
    }
    net.sta = esp_netif_create_default_wifi_sta();
    net.ap = esp_netif_create_default_wifi_ap();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&wc)) != ESP_OK ||
        (err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL)) != ESP_OK ||
        (err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL)) != ESP_OK ||
        (err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        ESP_LOGE(NET_TAG, "Wi-Fi init: %s", esp_err_to_name(err));
        return false;
    }
    wifi_config_t c;
    memset(&c, 0, sizeof(c));
    const bool legacy = esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK && c.sta.ssid[0];
    if (!profiles_load()) {
        ESP_LOGE(NET_TAG, "Wi-Fi profile store unavailable");
        memset(&c, 0, sizeof(c));
        return false;
    }
    if (legacy && !profiles_any()) {
        if (!net_store_network((const char *)c.sta.ssid, (const char *)c.sta.password)) {
            ESP_LOGE(NET_TAG, "could not migrate the saved Wi-Fi network");
            memset(&c, 0, sizeof(c));
            return false;
        }
        snprintf(net.st.ssid, sizeof(net.st.ssid), "%s", (const char *)c.sta.ssid);
    }
    /* ESP-IDF has one persistent station profile. It held the pre-0.10.0 credentials; now that
     * profile is migrated, clear it and keep per-attempt credentials in RAM. */
    wifi_config_t empty;
    memset(&empty, 0, sizeof(empty));
    (void)esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    (void)esp_wifi_set_config(WIFI_IF_STA, &empty);
    (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);
    memset(&c, 0, sizeof(c));
    net.st.wifi_state = profiles_any() ? PROTO_WIFI_STATE_IDLE : PROTO_WIFI_STATE_UNCONFIGURED;
    publish_networks();
    wx_init(&wx, now_ms());
    net.st.weather_state = wx_state(&wx, now_ms());
    net.gen = 1; /* the first net_poll reports the start state */
    if (xTaskCreate(task, "net", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        return false;
    }
    ESP_LOGI(NET_TAG, "Wi-Fi ready (%s)", net.st.ssid[0] ? "a network is stored" : "no network stored");
    return true;
}

/* ---------------------------------------------------------------- API (any task) */

bool net_configure(bool enabled, uint16_t interval_min, const uint8_t *city, size_t city_len, uint8_t language)
{
    cmd_t c = {.kind = CMD_CONFIG, .flag = enabled, .interval_min = interval_min, .language = language};
    c.city_len = city_len < WX_CITY_MAX ? city_len : WX_CITY_MAX;
    memcpy(c.city, city, c.city_len);
    return post(&c);
}

proto_status_t net_wifi_set(const uint8_t *ssid, size_t ssid_len, const uint8_t *pw, size_t pw_len, bool forget)
{
    cmd_t c = {.kind = CMD_WIFI_SET, .flag = forget};
    if (ssid_len > 32 || pw_len > 63) {
        return PROTO_OUT_OF_RANGE; /* the core checked; never expected */
    }
    if (ssid != NULL) {
        memcpy(c.ssid, ssid, ssid_len);
    }
    if (pw != NULL) {
        memcpy(c.pw, pw, pw_len);
    }
    return post(&c) ? PROTO_OK : PROTO_BUSY;
}

void net_setup(bool open)
{
    cmd_t c = {.kind = CMD_SETUP, .flag = open};
    (void)post(&c);
}

void net_kick(void)
{
    cmd_t c = {.kind = CMD_KICK};
    (void)post(&c);
}

bool net_post_save(const char *ssid, const char *pw, const char *city)
{
    cmd_t c = {.kind = CMD_SAVE};
    snprintf(c.ssid, sizeof(c.ssid), "%s", ssid);
    snprintf(c.pw, sizeof(c.pw), "%s", pw);
    snprintf(c.city, sizeof(c.city), "%s", city);
    return post(&c);
}
