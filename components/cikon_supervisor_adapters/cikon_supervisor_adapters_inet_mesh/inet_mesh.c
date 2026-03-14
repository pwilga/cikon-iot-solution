#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi_types_generic.h"

#include "bits_helper.h"
#include "cmnd.h"
#include "config_manager.h"
#include "inet_mesh_adapter.h"
#include "mesh_lite.h"
#include "mqtt.h"
#include "platform_services.h"
#include "supervisor.h"
#include "tcp_monitor.h"
#include "tcp_ota.h"
#include "tele.h"

#define TAG "cikon:adapter:inet_mesh"

static bool initialized = false;

static esp_event_handler_instance_t inet_mesh_wifi_handler = NULL;
static esp_event_handler_instance_t inet_mesh_ip_handler = NULL;

static void inet_mesh_netif_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                          void *event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        supervisor_notify_event(INET_EVENT_STA_READY);
    }

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            supervisor_notify_event(INET_EVENT_AP_READY);
            break;
        }
    }
}

static esp_err_t inet_mesh_adapter_init(void) {
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing inet_mesh adapter");

    mesh_lite_config_t mesh_cfg = {
        .mesh_id = config_get()->mesh_id,
        .sta_ssid = config_get()->wifi_ssid,
        .sta_password = config_get()->wifi_pass,
        .ap_ssid = config_get()->wifi_ap_ssid,
        .ap_password = config_get()->wifi_ap_pass,
    };
    mesh_lite_configure(&mesh_cfg);

    mesh_lite_init();

    const char *hostname = config_get()->mdns_host;
    if (strlen(hostname) == 0) {
        hostname = config_get()->dev_name;
    }

    static char device_url[64];
    snprintf(device_url, sizeof(device_url), "%s.local", hostname);

    mqtt_config_t mqtt_cfg = {.client_id = get_client_id(),
                              .device_name = config_get()->dev_name,
                              .device_manufacturer = "Cikon Systems",
                              .device_model = CONFIG_IDF_TARGET,
                              .device_sw_version = "v1.0.0",
                              .device_hw_version = CONFIG_IDF_INIT_VERSION,
                              .device_uri = device_url,
                              .mqtt_node = config_get()->mqtt_node,
                              .mqtt_broker = config_get()->mqtt_broker,
                              .mqtt_user = config_get()->mqtt_user,
                              .mqtt_pass = config_get()->mqtt_pass,
                              .mqtt_mtls_en = config_get()->mqtt_mtls_en,
                              .mqtt_max_retry = config_get()->mqtt_max_retry,
                              .mqtt_disc_pref = config_get()->mqtt_disc_pref,
                              .command_cb = cmnd_process_json,
                              .telemetry_cb = tele_append_all};

    mqtt_configure(&mqtt_cfg);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &inet_mesh_netif_event_handler, NULL,
                                                        &inet_mesh_wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &inet_mesh_netif_event_handler, NULL,
                                                        &inet_mesh_ip_handler));

    initialized = true;
    return ESP_OK;
}

static esp_err_t inet_mesh_adapter_shutdown(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Shutting down inet_mesh adapter");

    mqtt_publish_offline_state();
    mqtt_shutdown();

    if (inet_mesh_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, inet_mesh_ip_handler);
        inet_mesh_ip_handler = NULL;
    }

    if (inet_mesh_wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, inet_mesh_wifi_handler);
        inet_mesh_wifi_handler = NULL;
    }

    mesh_lite_shutdown();

    initialized = false;
    return ESP_OK;
}

static void inet_mesh_adapter_on_event(EventBits_t bits) {

    if (bits & SUPERVISOR_EVENT_CMND_COMPLETED) {
        mqtt_trigger_telemetry();
    }

    if (bits & INET_EVENT_STA_READY) {
        ESP_LOGI(TAG, "Mesh STA ready - connected to router");

        if (is_mesh_root_node()) {
            ESP_LOGI(TAG, "This node is MESH ROOT");
            tcp_ota_init();
            tcp_monitor_init();
        } else {
            ESP_LOGI(TAG, "This node is MESH CHILD");
        }
        if (!supervisor_is_safe_mode_active()) {
            mqtt_init();
        }
    }

    if (bits & INET_EVENT_AP_READY) {
        ESP_LOGI(TAG, "Mesh AP ready - accepting child nodes");
    }
}

static void inet_mesh_adapter_on_interval(supervisor_interval_stage_t stage) {
    // TODO: Add interval handling
}

supervisor_platform_adapter_t inet_mesh_adapter = {
    .name = "inet_mesh",
    .init = inet_mesh_adapter_init,
    .shutdown = inet_mesh_adapter_shutdown,
    .on_event = inet_mesh_adapter_on_event,
    .on_interval = inet_mesh_adapter_on_interval,
    .tele_group = NULL,
    .cmnd_group = NULL,
};
