#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "cmnd.h"
#include "cmnd_inet_handlers.h"
#include "ha.h"
#include "https_server.h"
#include "inet_adapter.h"
#include "json_parser.h"
#include "tcp_monitor.h"
#include "tcp_ota.h"

#define TAG "cmnd-inet-handlers"

static void set_ap_handler(const char *args_json_str) {
    (void)args_json_str;
    inet_switch_to_ap_mode();
}

static void set_sta_handler(const char *args_json_str) {
    (void)args_json_str;
    inet_switch_to_sta_mode();
}

static void https_handler(const char *args_json_str) {
    logic_state_t https_state = json_str_as_logic_state(args_json_str);

    if (https_state == STATE_ON) {
        ESP_LOGI(TAG, "Starting HTTPS server");
        https_init();
    } else if (https_state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping HTTPS server");
        https_shutdown();
    } else {
        ESP_LOGW(TAG, "Invalid HTTPS state");
    }
}

static void sntp_handler(const char *args_json_str) {
    logic_state_t sntp_state = json_str_as_logic_state(args_json_str);

    if (sntp_state == STATE_ON) {
        ESP_LOGI(TAG, "Starting SNTP service");
        esp_netif_sntp_deinit();
    } else if (sntp_state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping SNTP service");
        esp_netif_sntp_deinit();
    } else {
        ESP_LOGW(TAG, "Invalid SNTP state");
    }
}

static void ha_handler(const char *args_json_str) {
    logic_state_t force_empty_payload = json_str_as_logic_state(args_json_str);
    if (force_empty_payload == STATE_TOGGLE) {
        ESP_LOGE(TAG, "Toggling is not permitted for HA discovery");
        return;
    }

    ESP_LOGI(TAG, "Triggering Home Assistant MQTT discovery");
    publish_ha_mqtt_discovery(force_empty_payload == STATE_OFF);
}

static void ota_handler(const char *args_json_str) {
    logic_state_t ota_state = json_str_as_logic_state(args_json_str);

    if (ota_state == STATE_ON) {
        ESP_LOGI(TAG, "Starting OTA update");
        tcp_ota_init();
    } else if (ota_state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping OTA update");
        tcp_ota_shutdown();
    }
}

static void monitor_handler(const char *args_json_str) {
    logic_state_t monitor_state = json_str_as_logic_state(args_json_str);

    if (monitor_state == STATE_ON) {
        ESP_LOGI(TAG, "Starting TCP monitor");
        tcp_monitor_init();
    } else if (monitor_state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping TCP monitor");
        tcp_monitor_shutdown();
    }
}

static void wifi_handler(const char *args_json_str) {
    logic_state_t wifi_state = json_str_as_logic_state(args_json_str);

    if (wifi_state == STATE_ON) {
        ESP_LOGI(TAG, "Starting WiFi");
        inet_adapter.init();
    } else if (wifi_state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping WiFi");
        inet_adapter.shutdown();
    }
}

static const command_entry_t inet_commands[] = {
    {"ap", "Switch to AP mode", set_ap_handler},
    {"sta", "Switch to STA mode", set_sta_handler},
    {"https", "Control HTTPS server (on/off)", https_handler},
    {"sntp", "Control SNTP service (on/off)", sntp_handler},
    {"ota", "Control OTA service (on/off)", ota_handler},
    {"monitor", "Control TCP monitor (on/off)", monitor_handler},
    {"ha", "Trigger Home Assistant MQTT discovery", ha_handler},
    {NULL, NULL, NULL} // Sentinel
};

void inet_cmnd_handlers_register(void) {
    cmnd_register_group(inet_commands);

    // Register wifi control command separately (not unregistered with other commands)
    if (!cmnd_find("wifi")) {
        cmnd_register("wifi", "Control WiFi (on/off)", wifi_handler);
    }
}

void inet_cmnd_handlers_unregister(void) { cmnd_unregister_group(inet_commands); }