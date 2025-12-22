/* Homeassistant MQTT Discovery */
#pragma once

#include "metadata.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// NOLINTNEXTLINE(readability-identifier-naming)
typedef struct cJSON cJSON;

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
