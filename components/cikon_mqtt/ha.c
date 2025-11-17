#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "cJSON.h"

#include "ha.h"
#include "json_parser.h"
#include "mqtt.h"

#define TAG "home-assistant"
#define MAX_ENTITIES 32

static bool has_sent_full_dev = false;

// Entity definition structure
typedef struct {
    ha_entity_type_t type;
    const char *name;
    const char *device_class;
    ha_custom_builder_t custom_builder;
} ha_entity_def_t;

// Dynamic entity registry
static ha_entity_def_t entities[MAX_ENTITIES];
static size_t entity_count = 0;
static bool default_registered = false;

// Forward declarations
static void build_switch(cJSON *payload, const char *sanitized_name);
static void build_button(cJSON *payload, const char *sanitized_name);
static void build_tasks_dict(cJSON *payload, const char *sanitized_name);
static cJSON *create_ha_device(void);
static void register_default_entities(void);

static const char *get_type_str(ha_entity_type_t type) {
    switch (type) {
    case HA_SENSOR:
        return "sensor";
    case HA_SWITCH:
        return "switch";
    case HA_BUTTON:
        return "button";
    default:
        return "unknown";
    }
}

static cJSON *create_ha_device(void) {
    cJSON *device = cJSON_CreateObject();

    cJSON_AddStringToObject(device, "ids", mqtt_get_config()->client_id);

    if (has_sent_full_dev) {
        return device;
    }

    has_sent_full_dev = true;

    cJSON_AddStringToObject(device, "name", mqtt_get_config()->device_name);
    cJSON_AddStringToObject(device, "mf", mqtt_get_config()->device_manufacturer);
    cJSON_AddStringToObject(device, "mdl", mqtt_get_config()->device_model);
    cJSON_AddStringToObject(device, "hw", mqtt_get_config()->device_hw_version);
    cJSON_AddStringToObject(device, "sw", mqtt_get_config()->device_sw_version);

    // Safe: cJSON_AddStringToObject copies the string internally
    char config_url[32];
    snprintf(config_url, sizeof(config_url), "http://%s", mqtt_get_config()->device_ip_address);
    cJSON_AddStringToObject(device, "cu", config_url);

    return device;
}

static void build_switch(cJSON *payload, const char *sanitized_name) {
    char buf[64];

    snprintf(buf, sizeof(buf), "{\"%s\":true}", sanitized_name);
    cJSON_AddStringToObject(payload, "payload_on", buf);

    snprintf(buf, sizeof(buf), "{\"%s\":false}", sanitized_name);
    cJSON_AddStringToObject(payload, "payload_off", buf);

    cJSON_AddBoolToObject(payload, "state_on", true);
    cJSON_AddBoolToObject(payload, "state_off", false);
}

static void build_button(cJSON *payload, const char *sanitized_name) {
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"%s\": null}", sanitized_name);
    cJSON_AddStringToObject(payload, "command_template", buf);
}

static void build_tasks_dict(cJSON *payload, const char *sanitized_name) {
    char buf[64];

    snprintf(buf, sizeof(buf), "{{ value_json.%s | count }}", sanitized_name);
    cJSON_ReplaceItemInObject(payload, "val_tpl", cJSON_CreateString(buf));

    snprintf(buf, sizeof(buf), "{{ value_json.%s | tojson }}", sanitized_name);
    cJSON_AddStringToObject(payload, "json_attr_tpl", buf);
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
}

void ha_register_entity(ha_entity_type_t type, const char *name, const char *device_class,
                        ha_custom_builder_t custom_builder) {
    if (entity_count >= MAX_ENTITIES) {
        ESP_LOGE(TAG, "Maximum entity limit (%d) reached", MAX_ENTITIES);
        return;
    }

    entities[entity_count++] = (ha_entity_def_t){
        .type = type,
        .name = name,
        .device_class = device_class,
        .custom_builder = custom_builder,
    };
}

static void register_default_entities(void) {

    if (default_registered) {
        return;
    }
    default_registered = true;

    ha_register_entity(HA_SENSOR, "Temperature", "temperature", NULL);
    ha_register_entity(HA_SENSOR, "Uptime", "duration", NULL);
    ha_register_entity(HA_SENSOR, "Startup", "timestamp", NULL);
    ha_register_entity(HA_SWITCH, "Onboard Led", NULL, build_switch);
    ha_register_entity(HA_BUTTON, "Restart", NULL, build_button);
    ha_register_entity(HA_SENSOR, "Tasks Dict", NULL, build_tasks_dict);
}

static void publish_entity(const ha_entity_def_t *def, bool empty_payload) {

    char *sanitized_name = sanitize(def->name);

    char buf[128];
    char topic[128];
    char unique_id[64];

    // Build topic
    snprintf(unique_id, sizeof(unique_id), "%.6s_%s", mqtt_get_config()->client_id, sanitized_name);
    snprintf(topic, sizeof(topic), "%s/%s/%s/config", mqtt_get_config()->mqtt_disc_pref,
             get_type_str(def->type), unique_id);

    ESP_LOGI(TAG, "Topic: %s", topic);
    // Handle empty payload
    if (empty_payload) {
        ESP_LOGI(TAG, "Payload: (empty)");
        mqtt_publish(topic, "", 0, true);
        has_sent_full_dev = false;
        free(sanitized_name);
        return;
    }

    // Build payload
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "name", def->name);
    cJSON_AddStringToObject(payload, "uniq_id", unique_id);

    snprintf(buf, sizeof(buf), "%s/%s", mqtt_get_config()->mqtt_node, mqtt_get_config()->client_id);
    cJSON_AddStringToObject(payload, "~", buf);

    cJSON_AddStringToObject(payload, "stat_t", "~/tele");
    cJSON_AddStringToObject(payload, "cmd_t", "~/cmnd");
    cJSON_AddStringToObject(payload, "avty_t", "~/aval");

    snprintf(buf, sizeof(buf), "{{ value_json.%s  }}", sanitized_name);
    cJSON_AddStringToObject(payload, "val_tpl", buf);

    // Add device class if present
    if (def->device_class) {
        cJSON_AddStringToObject(payload, "dev_cla", def->device_class);
    }

    // Call custom builder if present
    if (def->custom_builder) {
        def->custom_builder(payload, sanitized_name);
    }

    cJSON_AddItemToObject(payload, "dev", create_ha_device());

    // Publish
    char *payload_str = cJSON_Print(payload);
    ESP_LOGI(TAG, "Payload: %s", payload_str);
    mqtt_publish(topic, payload_str, 0, true);

    cJSON_free(payload_str);
    cJSON_Delete(payload);
    free(sanitized_name);
}

void publish_ha_mqtt_discovery(bool force_empty_payload) {
    register_default_entities();

    for (size_t i = 0; i < entity_count; i++) {
        publish_entity(&entities[i], force_empty_payload);
    }
}