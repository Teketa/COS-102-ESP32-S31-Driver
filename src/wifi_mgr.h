// WiFi: station mode using stored credentials, with a SoftAP setup portal
// fallback when no credentials are stored or the connection keeps failing.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void wifi_mgr_init(void);
bool wifi_is_connected(void);
bool wifi_is_ap_mode(void);
const char *wifi_ip_str(void);
const char *wifi_ssid_str(void);
int  wifi_rssi(void);
// Store new credentials and reconnect (used by console + web API).
void wifi_set_credentials(const char *ssid, const char *pass);
void wifi_forget(void);
void wifi_set_txpower(int units);   // esp_wifi units = 0.25 dBm
int  wifi_get_txpower(void);
void time_sync_start(void);      // SNTP + timezone from settings
bool time_is_valid(void);
