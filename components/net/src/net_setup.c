/*
 * The Wi-Fi setup hotspot (PLAN D61), for phones without the app: opened from the pairing screen, a
 * WPA2 hotspot with a fresh random password (shown on the screen and as a QR code), a captive DNS that
 * points every name at the board, and one page that saves the network and the city. The board then
 * joins the network in AP+STA mode; the screen shows the outcome (the phone may drop off the hotspot
 * when the AP moves to the network's channel). Closes after success, on request, or when idle.
 * Everything the phone sends is parsed by host-tested code (net_util.c).
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "epager/net_util.h"
#include "epager/power.h"
#include "epager/proto_gen.h"
#include "net_priv.h"

#define PASSWORD_LEN 10
#define AP_MAX_CLIENTS 2
#define DNS_PORT 53
#define DNS_STACK 3072
#define HTTPD_STACK 6144
#define DONE_CLOSE_MS 8000            /* "Connected" stays on the screen this long, then the hotspot closes */
#define IDLE_CLOSE_MS (CONFIG_EPAGER_WIFI_SETUP_TIMEOUT_S * 1000u)
#define FORM_MAX 512

static struct {
    bool active;
    httpd_handle_t httpd;
    volatile bool dns_run;
    volatile bool dns_alive;
    volatile uint32_t last_request_ms;
    uint32_t done_ms;
    uint8_t ip[4];
    net_ap_t aps[NET_APS_MAX];
    size_t n_aps;
    char note[160];
} s;

static wifi_ap_record_t scan[NET_APS_MAX]; /* the Wi-Fi task's, kept off its stack */
static EXT_RAM_BSS_ATTR char page[NET_PAGE_MAX]; /* the page server's: one request at a time; PSRAM */

static const char *const NOTE_BAD_PW[] = {"The password must have 8 to 63 characters (or none).",
                                          "Mật khẩu phải có từ 8 đến 63 ký tự (hoặc để trống)."};
static const char *const NOTE_FAILED[] = {"Could not join that network. Check the password.",
                                          "Không kết nối được mạng đó. Hãy kiểm tra mật khẩu."};
static const char *const NOTE_NO_IP[] = {"Joined the network but got no internet address (weak signal?). Try "
                                         "again closer to the router.",
                                         "Đã vào mạng nhưng không nhận được địa chỉ internet (sóng yếu?). Hãy thử "
                                         "lại ở gần bộ phát hơn."};

static bool vietnamese(void) { return net.language == PROTO_LANGUAGE_VI; }

static void set_setup(net_setup_state_t state, const char *network)
{
    net_lock();
    net.st.setup = state;
    snprintf(net.st.setup_network, sizeof(net.st.setup_network), "%s", network ? network : "");
    net_unlock();
}

bool net_setup_active(void) { return s.active; }

/* ---------------------------------------------------------------- captive DNS */

static void dns_task(void *arg)
{
    (void)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(DNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    const struct timeval tv = {.tv_sec = 1};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        ESP_LOGE(NET_TAG, "captive DNS: no socket");
    } else {
        static uint8_t q[NET_DNS_MAX], r[NET_DNS_MAX];
        while (s.dns_run) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            const int n = recvfrom(sock, q, sizeof(q), 0, (struct sockaddr *)&from, &from_len);
            if (n > 0) {
                const size_t m = net_dns_answer(q, (size_t)n, s.ip, r, sizeof(r));
                if (m > 0) {
                    sendto(sock, r, m, 0, (struct sockaddr *)&from, from_len);
                }
            }
        }
    }
    if (sock >= 0) {
        close(sock);
    }
    s.dns_alive = false;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------- page server */

static esp_err_t send_page(httpd_req_t *req, size_t n)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, (ssize_t)n);
}

static esp_err_t send_form(httpd_req_t *req, const char *note)
{
    char device[33], ssid[33];
    xSemaphoreTake(net.lock, portMAX_DELAY); /* a read: no net_unlock, nothing changed */
    memcpy(device, net.st.ap_ssid, sizeof(device));
    memcpy(ssid, net.st.ssid, sizeof(ssid));
    xSemaphoreGive(net.lock);
    const net_page_t p = {.vietnamese = vietnamese(), .device = device, .aps = s.aps, .n_aps = s.n_aps,
                          .ssid = ssid, .city = net.city, .note = note};
    const size_t n = net_setup_page(page, sizeof(page), &p);
    return n ? send_page(req, n) : httpd_resp_send_500(req);
}

static esp_err_t on_root(httpd_req_t *req)
{
    s.last_request_ms = net.now_ms();
    return send_form(req, s.note[0] ? s.note : NULL);
}

static esp_err_t on_save(httpd_req_t *req)
{
    s.last_request_ms = net.now_ms();
    static char form[FORM_MAX + 1];
    if (req->content_len == 0 || req->content_len > FORM_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, form + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    char ssid[33], pw[64], city[WX_CITY_MAX + 1];
    const bool ok = net_form_get(form, got, "ssid", ssid, sizeof(ssid)) && ssid[0] &&
                    net_form_get(form, got, "pw", pw, sizeof(pw)) &&
                    net_form_get(form, got, "city", city, sizeof(city));
    memset(form, 0, sizeof(form)); /* the password */
    esp_err_t err;
    if (!ok || (pw[0] && strlen(pw) < 8)) {
        err = send_form(req, NOTE_BAD_PW[vietnamese()]);
    } else if (!net_post_save(ssid, pw, city)) {
        err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    } else {
        const size_t n = net_saved_page(page, sizeof(page), vietnamese(), ssid);
        err = n ? send_page(req, n) : httpd_resp_send_500(req);
    }
    memset(pw, 0, sizeof(pw));
    return err;
}

/* Every other address (the phones' captive-portal checks included) goes to the page. */
static esp_err_t on_other(httpd_req_t *req, httpd_err_code_t code)
{
    (void)code;
    char location[32];
    snprintf(location, sizeof(location), "http://%u.%u.%u.%u/", s.ip[0], s.ip[1], s.ip[2], s.ip[3]);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}

static bool start_server(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = HTTPD_STACK;
    c.max_open_sockets = 4;
    c.lru_purge_enable = true;
    if (httpd_start(&s.httpd, &c) != ESP_OK) {
        return false;
    }
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = on_root};
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = on_save};
    httpd_register_uri_handler(s.httpd, &root);
    httpd_register_uri_handler(s.httpd, &save);
    httpd_register_err_handler(s.httpd, HTTPD_404_NOT_FOUND, on_other);
    return true;
}

/* ---------------------------------------------------------------- open, close, join */

static void scan_networks(void)
{
    const wifi_scan_config_t sc = {.show_hidden = false};
    uint16_t n = NET_APS_MAX;
    s.n_aps = 0;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK || esp_wifi_scan_get_ap_records(&n, scan) != ESP_OK) {
        return;
    }
    for (uint16_t i = 0; i < n; i++) { /* strongest first; one entry per name */
        const char *name = (const char *)scan[i].ssid;
        bool dup = name[0] == '\0';
        for (size_t k = 0; k < s.n_aps && !dup; k++) {
            dup = strcmp(s.aps[k].ssid, name) == 0;
        }
        if (!dup) {
            net_ap_t *a = &s.aps[s.n_aps++];
            snprintf(a->ssid, sizeof(a->ssid), "%s", name);
            a->rssi = scan[i].rssi;
            a->open = scan[i].authmode == WIFI_AUTH_OPEN;
        }
    }
}

/* The hotspot serves phones for as long as it is open: no frequency scaling, no light sleep (PLAN D17). */
static void hold_power(void)
{
    power_hold(POWER_LOCK_CPU_MAX);
    power_hold(POWER_LOCK_NO_SLEEP);
}

bool net_setup_start(void)
{
    if (s.active) {
        return true;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    net_lock();
    snprintf(net.st.ap_ssid, sizeof(net.st.ap_ssid), "ePager-%02X%02X", mac[4], mac[5]);
    net_password(net.st.ap_password, PASSWORD_LEN, esp_random);
    net_unlock();

    wifi_config_t ap;
    memset(&ap, 0, sizeof(ap));
    memcpy(ap.ap.ssid, net.st.ap_ssid, strlen(net.st.ap_ssid));
    ap.ap.ssid_len = (uint8_t)strlen(net.st.ap_ssid);
    memcpy(ap.ap.password, net.st.ap_password, strlen(net.st.ap_password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = AP_MAX_CLIENTS;
    ap.ap.channel = 1;
    esp_netif_ip_info_t ip;
    esp_wifi_stop();
    /* The hotspot and all station attempts are session-only. ePager owns the three persisted profiles. */
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK ||
        esp_wifi_start() != ESP_OK || esp_netif_get_ip_info(net.ap, &ip) != ESP_OK) {
        ESP_LOGE(NET_TAG, "setup hotspot did not start");
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        return false;
    }
    memcpy(s.ip, &ip.ip.addr, 4); /* network byte order: a.b.c.d */
    net_lock();
    snprintf(net.st.setup_url, sizeof(net.st.setup_url), "%u.%u.%u.%u", s.ip[0], s.ip[1], s.ip[2], s.ip[3]);
    net_unlock();
    scan_networks();
    s.note[0] = '\0';
    s.dns_run = s.dns_alive = true;
    if (!start_server() || xTaskCreate(dns_task, "dns", DNS_STACK, NULL, 2, NULL) != pdPASS) {
        s.dns_alive = false;
        hold_power();
        s.active = true; /* so net_setup_stop undoes what started */
        net_setup_stop();
        return false;
    }
    hold_power();
    s.active = true;
    s.last_request_ms = net.now_ms();
    set_setup(NET_SETUP_WAITING, NULL);
    ESP_LOGI(NET_TAG, "setup hotspot open (%u networks around)", (unsigned)s.n_aps);
    return true;
}

void net_setup_stop(void)
{
    if (!s.active) {
        return;
    }
    s.dns_run = false;
    if (s.httpd != NULL) {
        httpd_stop(s.httpd);
        s.httpd = NULL;
    }
    for (int i = 0; i < 30 && s.dns_alive; i++) { /* its receive times out every second */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s.active = false;
    power_release(POWER_LOCK_NO_SLEEP);
    power_release(POWER_LOCK_CPU_MAX);
    net_lock();
    net.st.setup = NET_SETUP_OFF;
    net.st.ap_password[0] = '\0';
    if (net.st.wifi_state == PROTO_WIFI_STATE_CONNECTED) {
        net.st.wifi_state = PROTO_WIFI_STATE_IDLE; /* the radio is off again */
    }
    net_unlock();
    if (net.st.ssid[0]) {
        net_weather_kick();
    }
    ESP_LOGI(NET_TAG, "setup hotspot closed");
}

void net_setup_save(const char *ssid, const char *pw, const char *city)
{
    if (!s.active) {
        return;
    }
    set_setup(NET_SETUP_JOINING, ssid);
    net_lock();
    net.st.city_seq++;
    snprintf(net.st.city, sizeof(net.st.city), "%s", city);
    net_unlock();
    snprintf(net.city, sizeof(net.city), "%s", city);
    uint8_t state = PROTO_WIFI_STATE_NOT_FOUND; /* if the network cannot even be stored */
    if (!net_store_network(ssid, pw) || !net_join_named(ssid, NET_JOIN_TIMEOUT_MS, &state)) {
        net_lock();
        net.st.setup = NET_SETUP_FAILED;
        net.st.wifi_state = net.st.ssid[0] ? state : PROTO_WIFI_STATE_UNCONFIGURED;
        net_unlock();
        const char *const *note = state == PROTO_WIFI_STATE_NO_INTERNET ? NOTE_NO_IP : NOTE_FAILED;
        snprintf(s.note, sizeof(s.note), "%s", note[vietnamese()]);
        s.last_request_ms = net.now_ms(); /* the user gets the full time to try again */
        return;
    }
    esp_wifi_disconnect();
    net_lock();
    net.st.setup = NET_SETUP_DONE;
    net.st.wifi_state = PROTO_WIFI_STATE_CONNECTED;
    net_unlock();
    s.done_ms = net.now_ms();
}

void net_setup_service(void)
{
    const uint32_t now = net.now_ms();
    if (net.st.setup == NET_SETUP_DONE ? now - s.done_ms >= DONE_CLOSE_MS : now - s.last_request_ms >= IDLE_CLOSE_MS) {
        net_setup_stop();
    }
}
