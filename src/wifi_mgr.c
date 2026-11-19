#include "wifi_mgr.h"
#include "settings.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mdns.h"

#define TAG "wifi"
#define AP_SSID_PREFIX "COS102-Setup"
#define MAX_RETRY 8

static esp_netif_t *s_sta_netif, *s_ap_netif;
static bool s_connected, s_ap_mode;
static int s_retry;
static char s_ip[16] = "0.0.0.0";
static bool s_started;
static void mdns_start(void);

bool wifi_is_connected(void) { return s_connected; }
bool wifi_is_ap_mode(void) { return s_ap_mode; }
const char *wifi_ip_str(void) { return s_ip; }
const char *wifi_ssid_str(void) { return g_set.ssid; }

int wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

static void start_ap(void)
{
    ESP_LOGW(TAG, "starting setup AP");
    s_ap_mode = true;
    s_connected = false;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s-%02X%02X",
             AP_SSID_PREFIX, mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.channel = 6;
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    strcpy(s_ip, "192.168.4.1");
    ESP_LOGW(TAG, "connect to WiFi '%s' then open http://192.168.4.1", ap.ap.ssid);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (++s_retry <= MAX_RETRY) {
            ESP_LOGI(TAG, "reconnecting (%d/%d)", s_retry, MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        } else if (!s_ap_mode) {
            esp_wifi_stop();
            start_ap();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_ap_mode = false;
        s_retry = 0;
        ESP_LOGI(TAG, "connected, IP %s  ->  open http://%s", s_ip, s_ip);
        // No modem sleep: its periodic wake-ups add latency spikes that show
        // up as frame-timing jitter on the panel. The sign is mains powered.
        esp_wifi_set_ps(WIFI_PS_NONE);
        // Cap TX power. Every transmit is a current spike on the 3.3 V rail;
        // if the ESP32 shares a supply (or ground return) with the panel, that
        // spike modulates LED brightness. 13 dBm is ample for a home network
        // and roughly halves the peak draw versus full 20 dBm.
        esp_wifi_set_max_tx_power(g_set.wifi_txpower ? g_set.wifi_txpower : 52);
        time_sync_start();
        mdns_start();
    }
}

// mDNS: the sign answers as cos102.local, so the Mixxx bridge (and a
// browser) can find it without knowing its IP.
static void mdns_start(void)
{
    static bool started;
    if (started) return;
    if (mdns_init() != ESP_OK) { ESP_LOGW(TAG, "mDNS init failed"); return; }
    started = true;
    mdns_hostname_set("cos102");
    mdns_instance_name_set("COS-102 LED Sign");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_service_add(NULL, "_cos102", "_udp", 4210, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://cos102.local/");
}

static void start_sta(void)
{
    s_ap_mode = false;
    s_retry = 0;
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, g_set.ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, g_set.pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();
    ESP_LOGI(TAG, "connecting to '%s'", g_set.ssid);
}

void wifi_mgr_init(void)
{
    if (s_started) return;
    s_started = true;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, "cos102");

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi, NULL, NULL));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (g_set.ssid[0]) start_sta();
    else start_ap();
}

void wifi_set_credentials(const char *ssid, const char *pass)
{
    strlcpy(g_set.ssid, ssid, sizeof(g_set.ssid));
    strlcpy(g_set.pass, pass ? pass : "", sizeof(g_set.pass));
    settings_save();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    start_sta();
}

void wifi_forget(void)
{
    g_set.ssid[0] = 0;
    g_set.pass[0] = 0;
    settings_save();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    start_ap();
}

void wifi_set_txpower(int units)
{
    esp_wifi_set_max_tx_power((int8_t)units);
}

int wifi_get_txpower(void)
{
    int8_t p = 0;
    esp_wifi_get_max_tx_power(&p);
    return p;
}

static bool s_time_ok;
bool time_is_valid(void) { return s_time_ok; }

static void sntp_cb(struct timeval *tv) { s_time_ok = true; }

void time_sync_start(void)
{
    static bool inited;
    setenv("TZ", g_set.tz, 1);
    tzset();
    if (inited) return;
    inited = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = sntp_cb;
    esp_netif_sntp_init(&cfg);
}
