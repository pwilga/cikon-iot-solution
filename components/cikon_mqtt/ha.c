#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "cJSON.h"

#include "ha.h"
#include "json_parser.h"
#include "mqtt.h"

#define TAG "home-assistant"

static bool has_sent_full_dev = false;
static bool empty_payload = false;

cJSON *build_ha_device(void) {

    cJSON *device = cJSON_CreateObject();

    cJSON_AddStringToObject(device, "ids", mqtt_get_config()->client_id);

    if (has_sent_full_dev) {
        return device;
    }

    has_sent_full_dev = true;

    cJSON_AddStringToObject(device, "name", "Cikon MQTT Device");
    cJSON_AddStringToObject(device, "mf", "Cikon Systems");
    cJSON_AddStringToObject(device, "mdl", "ESP32 DevKit");
    cJSON_AddStringToObject(device, "hw", "ESP-IDF");
    cJSON_AddStringToObject(device, "sw", "0.0.1");

    //   const char *url = get_local_ip_url();
    //   if (url) {
    //     cJSON_AddStringToObject(device, "cu", url);
    //   }

    return device;
}

void build_ha_entity(ha_entity_t *entity, const char *entity_type, const char *name) {

    char *sanitized_name = sanitize(name);
    const size_t BUF_LEN = 128;

    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "%.6s_%s", mqtt_get_config()->client_id, sanitized_name);

    snprintf(entity->ha_config_topic, sizeof(entity->ha_config_topic), "%s/%s/%s/config",
             mqtt_get_config()->mqtt_disc_pref, entity_type, unique_id);

    if (empty_payload) {
        entity->ha_config_payload = NULL;
        has_sent_full_dev = false;
        return;
    }

    char buf[BUF_LEN];

    cJSON *json_root = cJSON_CreateObject();
    cJSON_AddStringToObject(json_root, "name", name);
    cJSON_AddStringToObject(json_root, "uniq_id", unique_id);

    snprintf(buf, sizeof(buf), "%s/%s", mqtt_get_config()->mqtt_node, mqtt_get_config()->client_id);
    cJSON_AddStringToObject(json_root, "~", buf);

    cJSON_AddStringToObject(json_root, "stat_t", "~/tele");
    cJSON_AddStringToObject(json_root, "cmd_t", "~/cmnd");
    cJSON_AddStringToObject(json_root, "avty_t", "~/aval");

    snprintf(buf, sizeof(buf), "{{ value_json.%s  }}", sanitized_name);
    cJSON_AddStringToObject(json_root, "val_tpl", buf);

    cJSON_AddItemToObject(json_root, "dev", build_ha_device());

    entity->ha_config_payload = json_root;

    free(sanitized_name);
}

void free_ha_entity(ha_entity_t *entity) {

    if (entity->ha_config_payload) {
        cJSON_Delete(entity->ha_config_payload);
        entity->ha_config_payload = NULL;
    }
}

/**
 * @brief Publishes a Home Assistant entity configuration via MQTT and frees its
 * resources.
 *
 * This function serializes the entity's JSON configuration payload (if present)
 * and publishes it to the associated MQTT topic using QoS 0. After publishing,
 * it frees the payload string and the internal resources held by the entity.
 *
 * @param entity Pointer to the ha_entity_t structure containing topic and
 * payload data.
 */
void submit_ha_entity(ha_entity_t *entity) {

    char *payload = NULL;

    if (entity->ha_config_payload)
        payload = cJSON_Print(entity->ha_config_payload);
    else
        payload = strdup("");

    ESP_LOGI(TAG, "Topic: %s", entity->ha_config_topic);
    ESP_LOGI(TAG, "Payload: %s", payload);

    mqtt_publish(entity->ha_config_topic, payload, 0, true);

    cJSON_free(payload);
    free_ha_entity(entity);
}

void register_ha_button(const char *name) {

    ha_entity_t entity;
    build_ha_entity(&entity, "button", name);

    if (entity.ha_config_payload) {

        char *sanitized_name = sanitize(name);
        char payload_command_buf[20];

        snprintf(payload_command_buf, sizeof(payload_command_buf), "{\"%s\": null}",
                 sanitized_name);
        free(sanitized_name);

        cJSON_AddStringToObject(entity.ha_config_payload, "command_template", payload_command_buf);
    }
    submit_ha_entity(&entity);
}

void register_ha_switch(const char *name) {

    ha_entity_t entity;
    build_ha_entity(&entity, "switch", name);

    if (entity.ha_config_payload) {

        char *sanitized_name = sanitize(name);
        char payload_onoff_buf[64];

        snprintf(payload_onoff_buf, sizeof(payload_onoff_buf), "{\"%s\":true}", sanitized_name);
        cJSON_AddStringToObject(entity.ha_config_payload, "payload_on", payload_onoff_buf);

        snprintf(payload_onoff_buf, sizeof(payload_onoff_buf), "{\"%s\":false}", sanitized_name);
        cJSON_AddStringToObject(entity.ha_config_payload, "payload_off", payload_onoff_buf);
        free(sanitized_name);

        cJSON_AddBoolToObject(entity.ha_config_payload, "state_on", true);
        cJSON_AddBoolToObject(entity.ha_config_payload, "state_off", false);
    }
    submit_ha_entity(&entity);
}

void register_ha_tasks_dict_sensor(const char *name) {

    ha_entity_t entity;
    build_ha_entity(&entity, "sensor", name);

    char *sanitized_name = sanitize(name);

    if (entity.ha_config_payload) {

        char val_buf[64];

        snprintf(val_buf, sizeof(val_buf), "{{ value_json.%s | count }}", sanitized_name);
        cJSON_ReplaceItemInObject(entity.ha_config_payload, "val_tpl", cJSON_CreateString(val_buf));

        snprintf(val_buf, sizeof(val_buf), "{{ value_json.%s | tojson }}", sanitized_name);
        cJSON_AddStringToObject(entity.ha_config_payload, "json_attr_tpl", val_buf);
        free(sanitized_name);

        cJSON_AddStringToObject(entity.ha_config_payload, "json_attr_t", "~/tele");
    }
    submit_ha_entity(&entity);
}

void publish_ha_mqtt_discovery(bool force_empty_payload) {

    empty_payload = force_empty_payload;

    ha_entity_t entity;

    build_ha_entity(&entity, "sensor", "Temperature");
    cJSON_AddStringToObject(entity.ha_config_payload, "dev_cla", "temperature");
    submit_ha_entity(&entity);

    build_ha_entity(&entity, "sensor", "Uptime");
    cJSON_AddStringToObject(entity.ha_config_payload, "dev_cla", "duration");
    submit_ha_entity(&entity);

    build_ha_entity(&entity, "sensor", "Startup");
    cJSON_AddStringToObject(entity.ha_config_payload, "dev_cla", "timestamp");
    submit_ha_entity(&entity);

    register_ha_switch("Onboard Led");
    register_ha_button("Restart");
    register_ha_tasks_dict_sensor("Tasks Dict");
}
