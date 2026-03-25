#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif_sntp.h"

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

bool inet_common_get_sta_ip(char *buf, size_t buflen);
bool inet_common_get_ap_ip(char *buf, size_t buflen);
void inet_common_log_ap_clients(void);

bool is_tcp_port_reachable(const char *ip, uint16_t port);
bool is_internet_reachable(void);

void inet_common_mdns_configure(const char *hostname, const char *instance);
void inet_common_mdns_init(void);
void inet_common_mdns_shutdown(void);

void inet_common_sntp_configure(const char **servers, esp_sntp_time_cb_t callback);
void inet_common_sntp_init(void);

#ifdef __cplusplus
}
#endif
