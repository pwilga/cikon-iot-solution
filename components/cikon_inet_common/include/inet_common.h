#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure MQTT with device configuration and callbacks
 *
 * Builds mqtt_config_t from device config and calls mqtt_configure().
 * Uses hostname from inet_common_get_hostname() for device URL.
 */
void inet_common_configure_mqtt(void);

/**
 * @brief Register all Home Assistant entities from adapter metadata
 *
 * Iterates through all supervisor adapters and registers their HA metadata.
 * Call this in SUPERVISOR_EVENT_PLATFORM_INITIALIZED handler.
 */
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
void inet_common_register_all_ha_entities(void);
#else
static inline void inet_common_register_all_ha_entities(void) {}
#endif

/**
 * @brief Home Assistant MQTT discovery command handler
 *
 * Triggers HA discovery publication. Accepts ON (publish) or OFF (unpublish).
 * Use this as command handler in adapter's cmnd_group.
 *
 * @param args_json_str Command payload ("on"/"off")
 */
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
void inet_common_ha_discovery_handler(const char *args_json_str);
#else
static inline void inet_common_ha_discovery_handler(const char *args_json_str) {}
#endif

const char *inet_common_get_hostname(void);
const char *inet_common_get_device_url(void);

#ifdef __cplusplus
}
#endif
