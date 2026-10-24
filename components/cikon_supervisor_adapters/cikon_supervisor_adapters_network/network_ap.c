#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "config_manager.h"
#include "http_server.h"
#include "json_parser.h"
#include "network_ap.h"
#include "wifi.h"

// Local service access point next to the uplink. Not an uplink itself: it only adds an interface
// the already running services answer on.
#if CONFIG_NETWORK_UPLINK_WIFI

#define TAG "cikon:network:ap"

#define NETWORK_AP_SCAN_MAX 20     // networks listed by GET /scan
#define NETWORK_AP_SCAN_CACHE_S 30 // GET /scan within this time returns the last results

// Only what the portal needs from wifi_ap_record_t (35 B instead of ~90 B per network)
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth;
} network_ap_scan_entry_t;

// Last scan results: a scan blocks HTTP and pauses STA attempts for ~3 s, so it runs at most once
// per NETWORK_AP_SCAN_CACHE_S no matter how often /scan is called
static network_ap_scan_entry_t scan_entries[NETWORK_AP_SCAN_MAX];
static uint8_t scan_entry_count = 0;
static int64_t scan_time_us = 0; // 0 = no scan yet

static void network_ap_handler(const char *args_json_str) {
    logic_state_t state = json_str_as_logic_state(args_json_str);

    if (state == STATE_TOGGLE) {
        wifi_mode_t mode = WIFI_MODE_NULL;
        esp_wifi_get_mode(&mode);
        state = (mode == WIFI_MODE_APSTA) ? STATE_OFF : STATE_ON;
    }

    if (state == STATE_ON) {
        wifi_ap_enable();
    } else {
        wifi_ap_disable();
    }
}

const command_entry_t network_ap_commands[] = {
    {"ap", "Control WiFi access point next to the uplink (on/off)", network_ap_handler},
    {NULL, NULL, NULL}};

// GET /scan for the WiFi setup portal: {"saved": "<ssid>", "aps": [{"ssid", "rssi", "auth"}]}
// Only networks STA can join (WPA2 or better). The portal itself sorts by rssi
static void network_ap_scan_json(cJSON *root) {
    int64_t now_us = esp_timer_get_time();
    int64_t age_s = (now_us - scan_time_us) / 1000000;

    if (scan_time_us == 0 || age_s >= NETWORK_AP_SCAN_CACHE_S) {
        wifi_ap_record_t records[NETWORK_AP_SCAN_MAX];
        uint16_t count = wifi_scan(records, NETWORK_AP_SCAN_MAX);

        scan_entry_count = 0;
        for (uint16_t i = 0; i < count; i++) {
            // STA accepts WPA2 at least - weaker networks would only fail to connect
            if (records[i].authmode < WIFI_AUTH_WPA2_PSK) {
                continue;
            }
            network_ap_scan_entry_t *entry = &scan_entries[scan_entry_count++];
            strlcpy(entry->ssid, (const char *)records[i].ssid, sizeof(entry->ssid));
            entry->rssi = records[i].rssi;
            entry->auth = records[i].authmode;
        }
        scan_time_us = now_us;
    } else {
        ESP_LOGI(TAG, "Scan skipped, returning results from %llds ago", age_s);
    }

    cJSON_AddStringToObject(root, "saved", config_get()->wifi_ssid);

    cJSON *aps = cJSON_AddArrayToObject(root, "aps");
    for (uint8_t i = 0; i < scan_entry_count; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", scan_entries[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", scan_entries[i].rssi);
        cJSON_AddNumberToObject(ap, "auth", scan_entries[i].auth);
        cJSON_AddItemToArray(aps, ap);
    }
}

void network_ap_http_register(void) { http_register_json_get("/scan", network_ap_scan_json); }

#endif
