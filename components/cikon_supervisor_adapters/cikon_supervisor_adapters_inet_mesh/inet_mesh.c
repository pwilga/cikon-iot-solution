#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi_types_generic.h"

#include "bits_helper.h"
#include "cmnd.h"
#include "config_manager.h"
#include "inet_common.h"
#include "inet_mesh_adapter.h"
#include "mesh_lite.h"
#include "mqtt.h"
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
#include "ha.h"
#endif
#include "supervisor.h"
#include "tcp_monitor.h"
#include "tcp_ota.h"
#include "tele.h"

#define TAG "cikon:adapter:inet_mesh"

static bool initialized = false;
static bool last_internet_reachable = false;

static esp_event_handler_instance_t inet_mesh_wifi_handler = NULL;
static esp_event_handler_instance_t inet_mesh_ip_handler = NULL;

// Mesh message callback
static void inet_mesh_on_message_received(cJSON *payload) {
    if (!payload)
        return;

    cJSON *cmnd = cJSON_GetObjectItem(payload, "cmnd");
    if (cmnd) {
        char *str = cJSON_PrintUnformatted(cmnd);
        cmnd_process_json(str);
        free(str);
        return;
    }

    if (cJSON_GetObjectItem(payload, "tele")) {
        // TODO: handle incoming telemetry
    }
}

static void inet_mesh_netif_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
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
        }
    }
}

static void inet_mesh_adapter_on_interval(supervisor_interval_stage_t stage) {

    if (stage == SUPERVISOR_INTERVAL_5S) {
        bool current_state = is_internet_reachable();

        // Only notify on state change
        if (current_state != last_internet_reachable) {
            if (current_state) {
                supervisor_notify_event(INET_INTERNET_READY);
            } else {
                supervisor_notify_event(INET_INTERNET_LOST);
            }
            last_internet_reachable = current_state;
        }
    }
}

static esp_err_t inet_mesh_adapter_init(void) {
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing inet_mesh adapter");

    // Register generic cmnd/tele dispatch BEFORE mesh init to avoid race condition
    mesh_lite_register_message_callback(inet_mesh_on_message_received);

    mesh_lite_config_t mesh_cfg = {
        .mesh_id = config_get()->mesh_id,
        .sta_ssid = config_get()->wifi_ssid,
        .sta_password = config_get()->wifi_pass,
        .ap_ssid = config_get()->wifi_ap_ssid,
        .ap_password = config_get()->wifi_ap_pass,
        .device_name = config_get()->dev_name,
    };
    mesh_lite_configure(&mesh_cfg);

    mesh_lite_init();

    inet_common_configure_mqtt();
    inet_common_sntp_configure(
        (const char *[]){config_get()->sntp1, config_get()->sntp2, config_get()->sntp3}, NULL);

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

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    if (bits & SUPERVISOR_EVENT_PLATFORM_INITIALIZED) {
        // Register all adapters' HA entities from metadata
        inet_common_register_all_ha_entities();

        // Register inet's own HA entity
        ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                                 .name = "IP",
                                                 .icon = "mdi:ip-outline",
                                                 .entity_category = "diagnostic"});
    }
#endif

    if (bits & SUPERVISOR_EVENT_CMND_COMPLETED) {
        mqtt_trigger_telemetry();
    }

    if (bits & INET_EVENT_STA_READY) {
        // ESP_LOGI(TAG, "Mesh STA ready - connected to router");

        if (is_mesh_root_node()) {
            ESP_LOGI(TAG, "This node is MESH ROOT");
            tcp_ota_init();
            tcp_monitor_init();
        } else {
            ESP_LOGI(TAG, "This node is MESH CHILD");
        }
        if (!supervisor_is_safe_mode_active()) {
            inet_common_sntp_init();
            mqtt_init();
        }
    }

    if (bits & INET_EVENT_AP_READY) {
        ESP_LOGI(TAG, "Mesh AP ready - accepting child nodes");
    }
}

static void tele_inet_mesh_ip_address(const char *tele_id, cJSON *json_root) {
    char ip[16];
    inet_common_get_sta_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(json_root, tele_id, ip);
}

static void cmnd_inet_mesh_send(const char *payload) {
    if (!payload || strlen(payload) == 0) {
        ESP_LOGW(TAG, "Empty payload for mesh send");
        return;
    }

    cJSON *msg = cJSON_Parse(payload);
    if (!msg) {
        ESP_LOGE(TAG, "Failed to parse mesh message payload: %s", payload);
        return;
    }

    if (!cJSON_GetObjectItem(msg, "source")) {
        cJSON_AddStringToObject(msg, "source", config_get()->dev_name);
    }

    ESP_LOGI(TAG, "Sending mesh message via command");
    esp_err_t ret = mesh_lite_send_message(msg);

    cJSON_Delete(msg);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send mesh message: %d", ret);
    }
}

static const command_entry_t inet_mesh_commands[] = {
    {"mesh_send", "Send mesh message (JSON payload)", cmnd_inet_mesh_send},
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    {"ha", "Trigger Home Assistant MQTT discovery", inet_common_ha_discovery_handler},
#endif
    {NULL, NULL, NULL}};

supervisor_platform_adapter_t inet_mesh_adapter = {
    .name = "inet_mesh",
    .init = inet_mesh_adapter_init,
    .shutdown = inet_mesh_adapter_shutdown,
    .on_event = inet_mesh_adapter_on_event,
    .on_interval = inet_mesh_adapter_on_interval,
    .tele_group = (const tele_entry_t[]){{"ip", tele_inet_mesh_ip_address}, {NULL, NULL}},
    .cmnd_group = inet_mesh_commands,
};
