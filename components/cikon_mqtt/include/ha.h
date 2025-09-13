/* Homeassistant MQTT Discovery */
#ifndef HA_H
#define HA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cJSON.h"
#include "stdbool.h"

typedef struct {
    char ha_config_topic[128];
    cJSON *ha_config_payload;
} ha_entity_t;

/**
 * @brief Builds a generic Home Assistant (HA) entity configuration for MQTT
 * Discovery.
 *
 * This function prepares a generic HA entity by:
 * - Constructing the appropriate MQTT discovery topic based on the entity name
 * and type.
 * - Creating the JSON payload with required fields for Home Assistant
 * auto-discovery.
 *
 * The entity structure (ha_entity_t) will be populated with the generated topic
 * and payload.
 *
 * @param entity Pointer to the ha_entity_t structure where the generated topic
 * and payload will be stored.
 * @param entity_type Type of the entity (e.g., "sensor", "switch",
 * "binary_sensor").
 * @param name Name of the entity (e.g., "living_room_light" or
 * "sensor_temperature").
 */
void build_ha_entity(ha_entity_t *entity, const char *entity_type, const char *name);
/**
 * @brief Frees the dynamically allocated Home Assistant entity configuration
 * payload.
 */
void free_ha_entity(ha_entity_t *entity);

/**
 * @brief Builds the Home Assistant MQTT Discovery "device" object.
 *
 * On the first call, returns the full device metadata including name,
 * model, software and hardware version, and configuration URL.
 * On subsequent calls, only the minimal "ids" field is returned.
 *
 * This allows devices to avoid sending duplicate metadata after
 * the initial discovery message, while still including required identifiers.
 *
 * @return A cJSON object representing the "device" field of the ha payload.
 *         Caller is responsible for freeing the object with cJSON_Delete().
 */
cJSON *build_ha_device(void);
void publish_ha_mqtt_discovery(bool force_empty_payload);

#ifdef __cplusplus
}
#endif

#endif // HA_H
