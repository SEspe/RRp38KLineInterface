/*
 * net.c - WiFi for the web HMI.
 *
 * Adapted from RemoteStart/SlaveWallas, with one structural difference:
 * there is no router in the car, so the access point is not a fallback
 * portal but runs all the time. A phone joins it and opens 192.168.4.1.
 * When home WiFi is configured the board also joins it as a station, which
 * is what installing from GitHub needs.
 *
 * AP+STA shares one radio. Every STA connect attempt scans all channels,
 * which drops the AP's clients for a moment, so retries back off and pause
 * entirely while a phone is connected to the AP (the in-car case).
 */
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"

#include "net.h"
#include "version.h"

static const char *TAG = "net";

#define STA_FAST_RETRIES     5           /* quick retries after boot or a drop */
#define STA_SLOW_RETRY_US    (60LL * 1000000)
#define STA_FAST_RETRY_US    (2LL * 1000000)

static esp_netif_t       *s_sta_netif;
static esp_timer_handle_t s_retry_timer;
static char     s_sta_ssid[33];
static char     s_ap_ssid[33];
static bool     s_sta_configured;
static volatile bool     s_sta_connected;
static volatile int      s_ap_clients;
static volatile uint32_t s_sta_disconnects;
static int      s_fast_left = STA_FAST_RETRIES;

/* ---------------------------------------------------------------- NVS -- */

static void nvs_get_string(const char *key, char *out, size_t len)
{
    nvs_handle_t h;
    out[0] = '\0';
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t l = len;
    if (nvs_get_str(h, key, out, &l) != ESP_OK) {
        out[0] = '\0';
    }
    nvs_close(h);
}

static esp_err_t nvs_set_strings(const char *k1, const char *v1,
                                 const char *k2, const char *v2)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, k1, v1);
    if (err == ESP_OK && k2) {
        err = nvs_set_str(h, k2, v2);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t net_save_sta(const char *ssid, const char *pass)
{
    return nvs_set_strings("ssid", ssid, "pass", pass);
}

esp_err_t net_save_ap_pass(const char *pass)
{
    size_t n = strlen(pass);
    if (n < 8 || n > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_set_strings("appass", pass, NULL, NULL);
}

/* ------------------------------------------------------------- events -- */

static void schedule_retry(void)
{
    if (!s_sta_configured) {
        return;
    }
    int64_t delay = STA_SLOW_RETRY_US;
    if (s_fast_left > 0) {
        s_fast_left--;
        delay = STA_FAST_RETRY_US;
    }
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, delay);
}

static void retry_cb(void *arg)
{
    if (s_sta_connected) {
        return;
    }
    if (s_ap_clients > 0) {
        /* Someone is using the HMI over the AP; a scan would drop them.
         * Look again later. */
        esp_timer_start_once(s_retry_timer, STA_SLOW_RETRY_US);
        return;
    }
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_sta_configured) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_sta_connected) {
                s_sta_disconnects++;
                s_fast_left = STA_FAST_RETRIES;
            }
            s_sta_connected = false;
            schedule_retry();
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            s_ap_clients++;
            ESP_LOGI(TAG, "phone joined the AP (%d connected)", s_ap_clients);
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_ap_clients > 0) {
                s_ap_clients--;
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_sta_connected = true;
        s_fast_left = STA_FAST_RETRIES;
        esp_timer_stop(s_retry_timer);
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "joined %s as " IPSTR, s_sta_ssid, IP2STR(&ev->ip_info.ip));
    }
}

/* --------------------------------------------------------------- start -- */

/* http://rrp38.local/ on the home network. Best-effort; never blocks
 * bring-up. On the AP the fixed address 192.168.4.1 is simpler anyway. */
static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set(FIRMWARE_NAME);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

esp_err_t net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, MDNS_HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "sta_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    /* Access point: always on. */
    char ap_pass[64];
    nvs_get_string("appass", ap_pass, sizeof(ap_pass));
    if (strlen(ap_pass) < 8) {
        strlcpy(ap_pass, WIFI_AP_PASSWORD, sizeof(ap_pass));
    }
    strlcpy(s_ap_ssid, WIFI_AP_SSID, sizeof(s_ap_ssid));
    wifi_config_t ap = { .ap = {
        .channel        = 1,
        .authmode       = WIFI_AUTH_WPA2_PSK,
        .max_connection = 4,
    }};
    ap.ap.ssid_len = strlen(s_ap_ssid);
    memcpy(ap.ap.ssid, s_ap_ssid, ap.ap.ssid_len);
    strlcpy((char *)ap.ap.password, ap_pass, sizeof(ap.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    /* Station: only if a home network has been saved. */
    char pass[64];
    nvs_get_string("ssid", s_sta_ssid, sizeof(s_sta_ssid));
    nvs_get_string("pass", pass, sizeof(pass));
    s_sta_configured = s_sta_ssid[0] != '\0';
    if (s_sta_configured) {
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, s_sta_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }

    start_mdns();
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Modem sleep adds real latency to HTTP; the board runs off the car or
     * USB, so there is no battery to save. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "AP \"%s\" up at 192.168.4.1%s%s", s_ap_ssid,
             s_sta_configured ? ", joining " : "", s_sta_configured ? s_sta_ssid : "");
    return ESP_OK;
}

void net_get_status(net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->ap_ssid, s_ap_ssid, sizeof(out->ap_ssid));
    out->ap_clients = s_ap_clients;
    strlcpy(out->sta_ssid, s_sta_ssid, sizeof(out->sta_ssid));
    out->sta_connected   = s_sta_connected;
    out->sta_disconnects = s_sta_disconnects;

    uint8_t primary = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
        out->ap_channel = primary;
    }
    if (s_sta_connected) {
        wifi_ap_record_t rec;
        if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
            out->sta_rssi = rec.rssi;
        }
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
            snprintf(out->sta_ip, sizeof(out->sta_ip), IPSTR, IP2STR(&ip.ip));
        }
    }
}
