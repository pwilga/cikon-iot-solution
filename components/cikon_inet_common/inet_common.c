#include <string.h>

#include "esp_log.h"

#include "cmnd.h"
#include "config_manager.h"
#include "json_parser.h"
#include "mqtt.h"
#include "supervisor.h"
#include "tele.h"
#include "wifi.h"
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
#include "ha.h"
#include "metadata.h"
#endif

#include "inet_common.h"

#define TAG "cikon:inet_common"

static char s_device_url[64];

void inet_common_configure_mqtt(void) {
    const char *device_url = inet_common_get_device_url();

    mqtt_config_t mqtt_cfg = {
        .client_id = get_client_id(),
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
        .telemetry_cb = tele_append_all,
    };

    mqtt_configure(&mqtt_cfg);
    ESP_LOGI(TAG, "MQTT configured (broker: %s, node: %s)", mqtt_cfg.mqtt_broker,
             mqtt_cfg.mqtt_node);
}

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
void inet_common_register_all_ha_entities(void) {
    ESP_LOGI(TAG, "Registering all HA entities from adapters");

    // Iterate through all adapters and register their HA metadata
    const supervisor_platform_adapter_t **adapters = supervisor_get_adapters();
    for (int i = 0; adapters[i] != NULL; i++) {
        if (adapters[i]->metadata == NULL) {
            continue;
        }

        const ha_metadata_t *meta = (const ha_metadata_t *)adapters[i]->metadata;

        // Check if this is HA metadata (magic signature)
        if (meta->magic != HA_METADATA_MAGIC) {
            continue;
        }

        // Register all entities from this adapter
        for (int e = 0; meta->entities[e].type != HA_ENTITY_NONE; e++) {
            ha_register_entity(&meta->entities[e]);
        }
    }
}

void inet_common_ha_discovery_handler(const char *args_json_str) {
    logic_state_t force_empty_payload = json_str_as_logic_state(args_json_str);
    if (force_empty_payload == STATE_TOGGLE) {
        ESP_LOGE(TAG, "Toggling is not permitted for HA discovery");
        return;
    }

    ESP_LOGI(TAG, "Triggering Home Assistant MQTT discovery");
    publish_ha_mqtt_discovery(force_empty_payload == STATE_OFF);
}
#endif

const char *inet_common_get_hostname(void) {
    const char *hostname = config_get()->mdns_host;
    if (strlen(hostname) == 0) {
        hostname = config_get()->dev_name;
    }
    return hostname;
}

const char *inet_common_get_device_url(void) {
    const char *hostname = inet_common_get_hostname();
    snprintf(s_device_url, sizeof(s_device_url), "%s.local", hostname);
    return s_device_url;
}
