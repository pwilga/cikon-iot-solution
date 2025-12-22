/**
 * @file metadata.h
 * @brief Platform metadata types for supervisor adapters
 *
 * This header provides type definitions for different platform integrations
 * (Home Assistant, Zigbee, Matter, etc.) that can be embedded in supervisor adapters.
 * Adapters define static metadata structures, and platform-specific adapters
 * (e.g., inet for HA, zigbee for Zigbee) iterate and register them at runtime.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Magic signatures for different metadata types
#define HA_METADATA_MAGIC 0x48414D44     // "HAMD" - Home Assistant
#define ZIGBEE_METADATA_MAGIC 0x5A494742 // "ZIGB" - Zigbee
#define MATTER_METADATA_MAGIC 0x4D545452 // "MTTR" - Matter

// Home Assistant Metadata

// NOLINTNEXTLINE(readability-identifier-naming)
typedef struct cJSON cJSON;

// Entity types
typedef enum {
    HA_ENTITY_NONE = 0,  // Sentinel for end of entity array
    HA_SENSOR,
    HA_SWITCH,
    HA_BUTTON,
    HA_LIGHT,
    HA_BINARY_SENSOR,
} ha_entity_type_t;

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
 * @brief Home Assistant metadata wrapper
 *
 * Use with adapter->metadata field:
 *
 * @example
 * .metadata = &(ha_metadata_t){
 *     .magic = HA_METADATA_MAGIC,
 *     .entities = {
 *         {.type = HA_SENSOR, .name = "temp", .device_class = "temperature"},
 *         {.type = 0}  // sentinel
 *     }
 * }
 */
typedef struct {
    uint32_t magic;                // Must be HA_METADATA_MAGIC
    ha_entity_config_t entities[]; // NULL-terminated (type = 0)
} ha_metadata_t;

// Future: Zigbee Metadata (placeholder)
// typedef struct {
//     uint32_t magic;  // ZIGBEE_METADATA_MAGIC
//     // ... zigbee endpoint definitions
// } zigbee_metadata_t;

// Future: Matter Metadata (placeholder)
// typedef struct {
//     uint32_t magic;  // MATTER_METADATA_MAGIC
//     // ... matter cluster definitions
// } matter_metadata_t;

#ifdef __cplusplus
}
#endif
