#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_types.h"
#include "esp_wifi_types_generic.h"

#include "bits_helper.h"
#include "cmnd.h"
#include "config_manager.h"
#include "platform_services.h"
#include "supervisor.h"
#include "tele.h"
#include "uplink.h"
#include "wifi.h"

#define TAG "cikon:uplink:wifi"

static esp_event_handler_instance_t wifi_event_instance = NULL;
static esp_event_handler_instance_t ip_event_instance = NULL;

static SemaphoreHandle_t network_transition_mutex = NULL;

static void switch_to_ap_mode(void) {
    if (xSemaphoreTake(network_transition_mutex, pdMS_TO_TICKS(100)) == pdFALSE) {
        ESP_LOGW(TAG, "Network transition already in progress, cannot switch to AP");
        return;
    }

    network_stop_services();
    wifi_init_ap_mode();

    xSemaphoreGive(network_transition_mutex);
}

static void switch_to_sta_mode(void) {
    if (xSemaphoreTake(network_transition_mutex, pdMS_TO_TICKS(100)) == pdFALSE) {
        ESP_LOGW(TAG, "Network transition already in progress, cannot switch to STA");
        return;
    }

    network_stop_services();
    wifi_init_sta_mode();

    xSemaphoreGive(network_transition_mutex);
}

static void uplink_wifi_on_restart(void) { wifi_unregister_event_handlers(); }

static void uplink_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                      void *event_data) {

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        supervisor_notify_event(INET_EVENT_STA_READY);
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_DISCONNECTED:
            supervisor_notify_event(INET_EVENT_STA_LOST);
            break;
        case WIFI_EVENT_AP_START:
            supervisor_notify_event(INET_EVENT_AP_READY);
            break;
        case WIFI_EVENT_AP_STOP:
            // Services stopped via callback before mode switch
            break;
        }
    }
}

static const char *get_or_generate_ap_ssid(void) {
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

    if (is_wifi_network_connected()) {
        ESP_LOGW(TAG, "WiFi network already connected, skipping WiFi uplink start");
        return ESP_ERR_INVALID_STATE;
    }

    if (network_transition_mutex == NULL) {
        network_transition_mutex = xSemaphoreCreateMutex();
        if (!network_transition_mutex) {
            ESP_LOGE(TAG, "Failed to create network transition mutex!");
        }
    }

    wifi_credentials_t creds = {.sta_ssid = config_get()->wifi_ssid,
                                .sta_password = config_get()->wifi_pass,
                                .ap_ssid = get_or_generate_ap_ssid(),
                                .ap_password = NULL};

    wifi_configure(&creds);

    register_restart_callback(uplink_wifi_on_restart);

    // Register callback for AP timeout -> STA mode switch
    wifi_set_ap_timeout_callback(network_stop_services);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &uplink_wifi_event_handler, NULL, &wifi_event_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &uplink_wifi_event_handler, NULL, &ip_event_instance));

    wifi_init_sta_mode();

    return ESP_OK;
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
    wifi_set_ap_timeout_callback(NULL);

    if (network_transition_mutex) {
        vSemaphoreDelete(network_transition_mutex);
        network_transition_mutex = NULL;
    }

    wifi_shutdown();

    return ESP_OK;
}

static void set_ap_handler(const char *args_json_str) {
    (void)args_json_str;
    switch_to_ap_mode();
}

static void set_sta_handler(const char *args_json_str) {
    (void)args_json_str;
    switch_to_sta_mode();
}

static void tele_wifi_ip_address(const char *tele_id, cJSON *json_root) {
    char ip[16];
    wifi_get_interface_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(json_root, tele_id, ip);
}

static void tele_wifi_rssi(const char *tele_id, cJSON *json_root) {
    int8_t rssi = wifi_get_rssi();
    if (rssi != 0)
        cJSON_AddNumberToObject(json_root, tele_id, rssi);
}

static void tele_wifi_ssid(const char *tele_id, cJSON *json_root) {
    char ssid[33];
    if (wifi_get_ssid(ssid, sizeof(ssid)))
        cJSON_AddStringToObject(json_root, tele_id, ssid);
}

static const command_entry_t wifi_commands[] = {
    {"ap", "Switch to AP mode", set_ap_handler},
    {"sta", "Switch to STA mode", set_sta_handler},
    {NULL, NULL, NULL}};

static const tele_entry_t wifi_tele[] = {
    {"ip",   tele_wifi_ip_address},
    {"rssi", tele_wifi_rssi},
    {"ssid", tele_wifi_ssid},
    {NULL, NULL}
};

const uplink_t uplink_wifi = {
    .name = "wifi",
    .start = uplink_wifi_start,
    .stop = uplink_wifi_stop,
    .commands = wifi_commands,
    .tele = wifi_tele,
};
