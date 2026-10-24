#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_wifi_types_generic.h"

#include "bits_helper.h"
#include "config_manager.h"
#include "http_server.h"
#include "network_ap.h"
#include "platform_services.h"
#include "supervisor.h"
#include "tele.h"
#include "uplink.h"
#include "wifi.h"

static esp_event_handler_instance_t wifi_event_instance = NULL;
static esp_event_handler_instance_t ip_event_instance = NULL;

static char portal_url[40]; // "http://<AP IP>/portal.html", kept by the HTTP server as a pointer

static void uplink_wifi_on_restart(void) { wifi_unregister_event_handlers(); }

static void uplink_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                      void *event_data) {

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        supervisor_notify_event(INET_EVENT_STA_READY | NETWORK_UPLINK_READY);
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_DISCONNECTED:
            supervisor_notify_event(INET_EVENT_STA_LOST | NETWORK_UPLINK_LOST);
            break;
        case WIFI_EVENT_AP_START: {
            supervisor_notify_event(INET_EVENT_AP_READY);

            // Phones check the internet on their own test address when they join: DNS sends it to
            // the device, and the missing page there redirects to the portal, which they then open
            esp_netif_ip_info_t ap_ip_info;
            esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ap_ip_info);
            network_ap_dns_start(ap_ip_info.ip);
            snprintf(portal_url, sizeof(portal_url), "http://" IPSTR "/portal.html",
                     IP2STR(&ap_ip_info.ip));
            http_set_not_found_redirect(portal_url);
            break;
        }
        case WIFI_EVENT_AP_STOP:
            network_ap_dns_stop();
            http_set_not_found_redirect(NULL);
            break;
        }
    }
}

static const char *uplink_wifi_get_or_generate_ap_ssid(void) {

    const char *cfg_ssid = config_get()->wifi_ap_ssid;
    if (cfg_ssid && strlen(cfg_ssid) > 0) {
        return cfg_ssid;
    }

    static char ssid[33];
    const char *mac_last6 = get_device_info()->id + 6;

    snprintf(ssid, sizeof(ssid), "%s_%s", config_get()->dev_name, mac_last6);
    return ssid;
}

static esp_err_t uplink_wifi_start(void) {

    wifi_credentials_t creds = {.sta_ssid = config_get()->wifi_ssid,
                                .sta_password = config_get()->wifi_pass,
                                .ap_ssid = uplink_wifi_get_or_generate_ap_ssid(),
                                .ap_password = NULL};

    register_restart_callback(uplink_wifi_on_restart);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &uplink_wifi_event_handler, NULL, &wifi_event_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &uplink_wifi_event_handler, NULL, &ip_event_instance));

    return wifi_start(&creds);
}

static esp_err_t uplink_wifi_stop(void) {

    if (ip_event_instance) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_instance);
        ip_event_instance = NULL;
    }

    if (wifi_event_instance) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_instance);
        wifi_event_instance = NULL;
    }

    unregister_restart_callback(uplink_wifi_on_restart);

    // The handler is gone, so AP_STOP from the shutdown below wouldn't reach it
    network_ap_dns_stop();
    http_set_not_found_redirect(NULL);

    wifi_shutdown();

    return ESP_OK;
}

static void uplink_wifi_tele_ip_address(const char *tele_id, cJSON *json_root) {
    char ip[16];
    wifi_get_interface_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(json_root, tele_id, ip);
}

static void uplink_wifi_tele_rssi(const char *tele_id, cJSON *json_root) {
    int8_t rssi = wifi_get_rssi();
    if (rssi != 0)
        cJSON_AddNumberToObject(json_root, tele_id, rssi);
}

static void uplink_wifi_tele_ssid(const char *tele_id, cJSON *json_root) {
    char ssid[33];
    if (wifi_get_ssid(ssid, sizeof(ssid)))
        cJSON_AddStringToObject(json_root, tele_id, ssid);
}

// Connect STA with the credentials currently in config, e.g. right after setconf
static void uplink_wifi_sta_connect_handler(const char *args_json_str) {
    wifi_sta_connect(config_get()->wifi_ssid, config_get()->wifi_pass);
}

static const command_entry_t uplink_wifi_commands[] = {
    {"sta_connect", "Connect WiFi STA with the credentials from config",
     uplink_wifi_sta_connect_handler},
    {NULL, NULL, NULL}};

static const tele_entry_t uplink_wifi_tele[] = {{"ip", uplink_wifi_tele_ip_address},
                                                {"rssi", uplink_wifi_tele_rssi},
                                                {"ssid", uplink_wifi_tele_ssid},
                                                {NULL, NULL}};

const uplink_t uplink_wifi = {
    .name = "wifi",
    .start = uplink_wifi_start,
    .stop = uplink_wifi_stop,
    .commands = uplink_wifi_commands,
    .tele = uplink_wifi_tele,
};
