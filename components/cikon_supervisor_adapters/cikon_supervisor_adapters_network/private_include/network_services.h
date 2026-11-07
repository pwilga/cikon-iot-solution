#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/event_groups.h"

#include "esp_netif_sntp.h"
#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
void network_services_register_all_ha_entities(void);
#else
static inline void network_services_register_all_ha_entities(void) {}
#endif

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
void network_services_ha_discovery_handler(const char *args_json_str);
#else
static inline void network_services_ha_discovery_handler(const char *args_json_str) {}
#endif

void network_services_on_interval(supervisor_interval_stage_t stage);
void network_services_on_event(EventBits_t bits);
void network_services_on_restart(void);

const char *network_services_get_hostname(void);
const char *network_services_get_device_url(void);

bool network_services_get_netif_ip(const char *if_key, char *buf, size_t buflen);

bool network_services_is_tcp_port_reachable(const char *ip, uint16_t port);
bool network_services_is_internet_reachable(void);
void network_services_poll_internet_reachability(void);

void network_services_mdns_init(void);
void network_services_mdns_shutdown(void);

void network_services_sntp_init(void);
static inline void network_services_sntp_shutdown(void) {
    extern void esp_netif_sntp_deinit(void);
    esp_netif_sntp_deinit();
}

void network_services_mqtt_init(void);
static inline void network_services_mqtt_shutdown(void) {
    extern void mqtt_shutdown(void);
    mqtt_shutdown();
}

void network_services_http_init(bool secure);
void network_services_http_start(void);
void network_services_ota_start(void);
void network_services_monitor_start(void);
void network_services_service_handler(const char *args_json_str);
static inline void network_services_http_shutdown(void) {
    extern void http_shutdown(void);
    http_shutdown();
}

#ifdef __cplusplus
}
#endif
