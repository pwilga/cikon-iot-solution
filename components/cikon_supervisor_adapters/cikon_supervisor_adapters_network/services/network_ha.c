#include "esp_log.h"

#include "ha.h"
#include "json_parser.h"
#include "metadata.h"
#include "network_services.h"
#include "platform_services.h"
#include "supervisor.h"

#define TAG "cikon:network:ha"

#if CONFIG_SUPERVISOR_TELE_TASKS
static void build_tasks_dict_ha(cJSON *payload, const char *sanitized_name) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{{ value_json.%s | count }}", sanitized_name);
    cJSON_ReplaceItemInObject(payload, "val_tpl", cJSON_CreateString(buf));
    snprintf(buf, sizeof(buf), "{{ value_json.%s | tojson }}", sanitized_name);
    cJSON_AddStringToObject(payload, "json_attr_tpl", buf);
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
}
#endif

static void build_chip_attrs(cJSON *payload, const char *sanitized_name) {
    (void)sanitized_name;
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
    cJSON_AddStringToObject(payload, "json_attr_tpl",
                            "{{ {'features': value_json.features, 'cores': value_json.cores, "
                            "'chip_rev': value_json.chip_rev, 'cpu_freq': value_json.cpu_freq, "
                            "'flash_size': value_json.flash_size, "
                            "'psram_size': value_json.psram_size | default(0)} "
                            "| tojson }}");
}

static void build_fs_attrs(cJSON *payload, const char *sanitized_name) {
    (void)sanitized_name;
    cJSON_ReplaceItemInObject(
        payload, "val_tpl",
        cJSON_CreateString("{{ (100 * value_json.fs_used / value_json.fs_size) | round(1) }}"));
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
    cJSON_AddStringToObject(payload, "json_attr_tpl",
                            "{{ {'fs_used': value_json.fs_used, "
                            "'fs_size': value_json.fs_size} | tojson }}");
}

static void build_dram_attrs(cJSON *payload, const char *sanitized_name) {
    (void)sanitized_name;
    cJSON_ReplaceItemInObject(
        payload, "val_tpl",
        cJSON_CreateString(
            "{{ (100 * (1 - value_json.dram_free / value_json.dram_size)) | round(1) }}"));
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
    cJSON_AddStringToObject(payload, "json_attr_tpl",
                            "{{ {'dram_free': value_json.dram_free, "
                            "'dram_min_free': value_json.dram_min_free, "
                            "'dram_size': value_json.dram_size} | tojson }}");
}

static void build_psram_attrs(cJSON *payload, const char *sanitized_name) {
    (void)sanitized_name;
    cJSON_ReplaceItemInObject(
        payload, "val_tpl",
        cJSON_CreateString(
            "{{ (100 * (1 - value_json.psram_free / value_json.psram_size)) | round(1) }}"));
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
    cJSON_AddStringToObject(payload, "json_attr_tpl",
                            "{{ {'psram_free': value_json.psram_free, "
                            "'psram_min_free': value_json.psram_min_free, "
                            "'psram_size': value_json.psram_size} | tojson }}");
}

static void build_app_build_time_attrs(cJSON *payload, const char *sanitized_name) {
    (void)sanitized_name;
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
    cJSON_AddStringToObject(payload, "json_attr_tpl",
                            "{{ {'version': value_json.version, 'idf': value_json.idf} "
                            "| tojson }}");
}

static void build_bootloader_build_time_attrs(cJSON *payload, const char *sanitized_name) {
    (void)sanitized_name;
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
    cJSON_AddStringToObject(payload, "json_attr_tpl",
                            "{{ {'version': value_json.bootloader_version, "
                            "'idf': value_json.bootloader_idf} | tojson }}");
}

static void register_core_ha_entities(void) {
    // Runtime
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "Boot Time",
                                             .device_class = "timestamp",
                                             .icon = "mdi:clock-start",
                                             .entity_category = "diagnostic"});
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "DRAM",
                                             .unit = "%",
                                             .icon = "mdi:memory",
                                             .entity_category = "diagnostic",
                                             .custom_builder = build_dram_attrs});
    if (get_psram_size() > 0) {
        ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                                 .name = "PSRAM",
                                                 .unit = "%",
                                                 .icon = "mdi:memory",
                                                 .entity_category = "diagnostic",
                                                 .custom_builder = build_psram_attrs});
    }

    // Device info
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "Chip",
                                             .icon = "mdi:chip",
                                             .entity_category = "diagnostic",
                                             .custom_builder = build_chip_attrs});
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "ID",
                                             .icon = "mdi:identifier",
                                             .entity_category = "diagnostic"});
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "App Build Time",
                                             .device_class = "timestamp",
                                             .icon = "mdi:wrench-clock",
                                             .entity_category = "diagnostic",
                                             .custom_builder = build_app_build_time_attrs});

    // Bootloader
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "Bootloader Build Time",
                                             .device_class = "timestamp",
                                             .icon = "mdi:clock-outline",
                                             .entity_category = "diagnostic",
                                             .custom_builder = build_bootloader_build_time_attrs});

    // OTA
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "OTA State",
                                             .icon = "mdi:cloud-upload-outline",
                                             .entity_category = "diagnostic"});

    // Hardware
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "Reset Reason",
                                             .icon = "mdi:restart-alert",
                                             .entity_category = "diagnostic"});
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "FS",
                                             .unit = "%",
                                             .icon = "mdi:folder-outline",
                                             .entity_category = "diagnostic",
                                             .custom_builder = build_fs_attrs});
    float chip_temp;
    if (get_chip_temp(&chip_temp)) {
        ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                                 .name = "Chip Temp",
                                                 .device_class = "temperature",
                                                 .unit = "°C",
                                                 .icon = "mdi:thermometer",
                                                 .entity_category = "diagnostic"});
    }

    // Controls
    ha_register_entity(&(ha_entity_config_t){.type = HA_BUTTON,
                                             .name = "Restart",
                                             .device_class = "restart",
                                             .icon = "mdi:restart",
                                             .entity_category = "diagnostic"});
}

void network_services_register_all_ha_entities(void) {
    ESP_LOGI(TAG, "Registering all HA entities from adapters");

    register_core_ha_entities();

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

#if CONFIG_SUPERVISOR_TELE_TASKS
    ha_register_entity(&(ha_entity_config_t){
        .type = HA_SENSOR,
        .name = "Tasks Dict",
        .entity_category = "diagnostic",
        .custom_builder = build_tasks_dict_ha,
    });
#endif
}

void network_services_ha_discovery_handler(const char *args_json_str) {
    logic_state_t force_empty_payload = json_str_as_logic_state(args_json_str);
    if (force_empty_payload == STATE_TOGGLE) {
        ESP_LOGE(TAG, "Toggling is not permitted for HA discovery");
        return;
    }

    ESP_LOGI(TAG, "Triggering Home Assistant MQTT discovery");
    publish_ha_mqtt_discovery(force_empty_payload == STATE_OFF);
}
