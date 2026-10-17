#include "esp_log.h"

#include "bits_helper.h"
#include "cmnd.h"
#include "config_manager.h"
#include "inet_common.h"
#include "metadata.h"
#include "mqtt.h"
#include "network_adapter.h"
#include "platform_services.h"
#include "supervisor.h"
#include "tcp_monitor.h"
#include "tcp_ota.h"
#include "tele.h"
#include "uplink.h"

#define TAG "cikon:adapter:network"

static const uplink_t *const uplinks[] = {
#if CONFIG_NETWORK_UPLINK_WIFI
    &uplink_wifi,
#endif
};

_Static_assert(sizeof(uplinks) / sizeof(uplinks[0]) > 0, "network adapter: no uplink enabled");

static const uplink_t *uplink = NULL;
static bool initialized = false;

static volatile bool sta_services_running = false;
static volatile bool ap_services_running = false;

void network_stop_services(void) {
    ESP_LOGI(TAG, "Stopping network services");

    // CRITICAL: Shutdown tcp_monitor FIRST before network stack resets
    tcp_monitor_shutdown();

    mqtt_publish_offline_state();
    inet_common_mqtt_shutdown();
    inet_common_http_shutdown();
    // Keep mDNS running - it works for both STA and AP interfaces
    // mdns_free();
    inet_common_sntp_shutdown();

    if (!restart_pending()) {
        tcp_ota_shutdown();
    }

    sta_services_running = false;
    ap_services_running = false;
}

static esp_err_t network_adapter_init(void) {

    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uplink = uplinks[0];
    ESP_LOGI(TAG, "Initializing network adapter, uplink: %s", uplink->name);

    esp_err_t err = uplink->start();
    if (err != ESP_OK) {
        return err;
    }

    if (uplink->commands) {
        cmnd_register_group(uplink->commands);
    }
    if (uplink->tele) {
        tele_register_group(uplink->tele);
    }

    register_restart_callback(inet_common_on_restart);

    initialized = true;
    return ESP_OK;
}

static esp_err_t network_adapter_shutdown(void) {

    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Shutting down network adapter");

    network_stop_services();
    inet_common_mdns_shutdown();

    unregister_restart_callback(inet_common_on_restart);

    if (uplink->commands) {
        cmnd_unregister_group(uplink->commands);
    }
    if (uplink->tele) {
        tele_unregister_group(uplink->tele);
    }

    uplink->stop();

    initialized = false;
    return ESP_OK;
}

static void network_adapter_on_event(EventBits_t bits) {

    inet_common_on_event(bits);

    if (bits & INET_EVENT_STA_READY) {

        if (sta_services_running) {
            ESP_LOGW(TAG, "STA services already running, ignoring");
            if (!supervisor_is_safe_mode_active()) {
                mqtt_init();
            }
            return;
        }

        ESP_LOGI(TAG, "STA ready, starting services");
        tcp_ota_init();
        tcp_monitor_init();

        if (!supervisor_is_safe_mode_active()) {
            inet_common_mdns_init();
            inet_common_sntp_init();
            inet_common_mqtt_init();
            inet_common_http_init(config_get()->http_secure);
        }

        sta_services_running = true;
        ap_services_running = false;
    }

    if (bits & INET_EVENT_AP_READY) {
        if (ap_services_running) {
            ESP_LOGW(TAG, "AP services already running, ignoring duplicate event");
            return;
        }

        ESP_LOGI(TAG, "AP ready, starting services");
        tcp_ota_init();
        tcp_monitor_init();

        if (!supervisor_is_safe_mode_active()) {
            inet_common_http_init(true);
            inet_common_mdns_init();
        }

        ap_services_running = true;
        sta_services_running = false;
    }
}

static void network_adapter_on_interval(supervisor_interval_stage_t stage) {
    inet_common_on_interval(stage);

    if (stage == SUPERVISOR_INTERVAL_10M && sta_services_running) {
        mqtt_init();
    }
}

static void tele_network_link(const char *tele_id, cJSON *json_root) {
    if (uplink) {
        cJSON_AddStringToObject(json_root, tele_id, uplink->name);
    }
}

static const tele_entry_t network_tele[] = {
    {"link", tele_network_link},
    {NULL, NULL}
};

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
static void build_link_attrs(cJSON *payload, const char *sanitized_name) {
    (void)sanitized_name;
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
    cJSON_AddStringToObject(payload, "json_attr_tpl",
                            "{{ {'rssi': value_json.rssi | default(0), "
                            "'ssid': value_json.ssid | default('')} "
                            "| tojson }}");
}

static const ha_metadata_t network_ha_metadata = {
    .magic = HA_METADATA_MAGIC,
    .entities = {{.type = HA_SENSOR,
                 .name = "Link",
                 .icon = "mdi:lan-connect",
                 .entity_category = "diagnostic",
                 .custom_builder = build_link_attrs},
                {.type = HA_ENTITY_NONE}},
};
#endif

static const command_entry_t network_commands[] = {
    {"http", "Control HTTP server (on/off)", inet_common_http_handler},
    {"sntp", "Control SNTP service (on/off)", inet_common_sntp_handler},
    {"ota", "Control OTA service (on/off)", inet_common_ota_handler},
    {"monitor", "Control TCP monitor (on/off)", inet_common_monitor_handler},
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    {"ha", "Trigger Home Assistant MQTT discovery", inet_common_ha_discovery_handler},
#endif
    {NULL, NULL, NULL}};

supervisor_platform_adapter_t network_adapter = {
    .enable_in_safe_mode = true, // Critical: always init network adapter
    .name = "network",
    .init = network_adapter_init,
    .shutdown = network_adapter_shutdown,
    .on_event = network_adapter_on_event,
    .on_interval = network_adapter_on_interval,
    .cmnd_group = network_commands,
    .tele_group = network_tele,
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    .metadata = &network_ha_metadata,
#endif
};
