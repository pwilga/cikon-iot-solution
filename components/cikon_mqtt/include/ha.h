/* Homeassistant MQTT Discovery */
#ifndef HA_H
#define HA_H

#include "cJSON.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Entity types
typedef enum {
    HA_SENSOR,
    HA_SWITCH,
    HA_BUTTON,
} ha_entity_type_t;

// Custom builder callback type
typedef void (*ha_custom_builder_t)(cJSON *payload, const char *sanitized_name);

/**
 * @brief Register a Home Assistant entity.
 *
 * @param type Entity type (HA_SENSOR, HA_SWITCH, HA_BUTTON, ...)
 * @param name Human-readable entity name
 * @param device_class Device class (e.g., "temperature", "duration") or NULL
 * @param custom_builder Optional custom payload builder callback or NULL
 */
void ha_register_entity(ha_entity_type_t type, const char *name, const char *device_class,
                        ha_custom_builder_t custom_builder);

/**
 * @brief Publishes all registered Home Assistant entities via MQTT Discovery.
 *
 * Default entities are auto-registered on first call. User can register
 * additional entities before calling this function.
 *
 * @param force_empty_payload If true, publishes empty payloads to remove entities
 */
void publish_ha_mqtt_discovery(bool force_empty_payload);

#ifdef __cplusplus
}
#endif

#endif // HA_H