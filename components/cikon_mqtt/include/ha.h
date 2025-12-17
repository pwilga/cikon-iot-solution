/* Homeassistant MQTT Discovery */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// NOLINTNEXTLINE(readability-identifier-naming)
typedef struct cJSON cJSON;

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
 * @brief Home Assistant entity configuration structure
 */
typedef struct {
    ha_entity_type_t type;              // Required: Entity type
    const char *name;                   // Required: Human-readable name
    const char *device_class;           // Optional: Device class ("temperature", "duration", etc.)
    const char *entity_category;        // Optional: "diagnostic" or "config" (NULL for main)
    const char *parent_key;             // Optional: Parent key for nested JSON (e.g., "temps" for
                                        // {"temps":{"temp0":23.5}})
    const char *icon;                   // Optional: Icon name (e.g., "mdi:thermometer")
    const char *unit;                   // Optional: Unit of measurement (e.g., "°C", "s")
    ha_custom_builder_t custom_builder; // Optional: Custom payload builder
} ha_entity_config_t;

/**
 * @brief Register a Home Assistant entity.
 *
 * @param config Pointer to entity configuration struct
 *
 * @note The name will be sanitized (spaces → underscores) for MQTT topic keys,
 *       but the original name is preserved for display in Home Assistant UI.
 *
 * @example
 * ha_register_entity(&(ha_entity_config_t){
 *     .type = HA_SENSOR,
 *     .name = "temp0",
 *     .device_class = "temperature",
 *     .parent_key = "temps"
 * });
 */
void ha_register_entity(const ha_entity_config_t *config);

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
