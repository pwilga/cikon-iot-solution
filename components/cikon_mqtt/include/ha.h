/* Homeassistant MQTT Discovery */
#pragma once

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
    HA_LIGHT,
} ha_entity_type_t;

// Custom builder callback type
typedef void (*ha_custom_builder_t)(cJSON *payload, const char *sanitized_name);

/**
 * @brief Register a Home Assistant entity.
 *
 * @param type Entity type (HA_SENSOR, HA_SWITCH, HA_BUTTON, HA_LIGHT)
 * @param name Human-readable entity name (can contain spaces)
 * @param device_class (Optional) Device class (e.g., "temperature", "duration") or NULL
 * @param entity_category (Optional) Entity category or NULL:
 *                        - NULL: Main controls/sensors (sections: "Controls", "Sensors")
 *                        - "diagnostic": Diagnostic data (section: "Diagnostics")
 *                        - "config": Configuration settings (section: "Configuration")
 * @param custom_builder (Optional) Custom payload builder callback or NULL.
 *                       If NULL, default builders are used based on entity type.
 *
 * @note The name will be sanitized (spaces → underscores) for MQTT topic keys,
 *       but the original name is preserved for display in Home Assistant UI.
 */
void ha_register_entity(ha_entity_type_t type, const char *name, const char *device_class,
                        const char *entity_category, ha_custom_builder_t custom_builder);

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
