#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/event_groups.h"

#include "esp_netif_sntp.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
void inet_common_register_all_ha_entities(void);
#else
static inline void inet_common_register_all_ha_entities(void) {}
#endif

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
void inet_common_ha_discovery_handler(const char *args_json_str);
#else
static inline void inet_common_ha_discovery_handler(const char *args_json_str) {}
#endif

void inet_common_on_event(EventBits_t bits);

const char *inet_common_get_hostname(void);
const char *inet_common_get_device_url(void);

bool inet_common_get_sta_ip(char *buf, size_t buflen);
bool inet_common_get_ap_ip(char *buf, size_t buflen);
void inet_common_log_ap_clients(void);

bool is_tcp_port_reachable(const char *ip, uint16_t port);
bool is_internet_reachable(void);
void inet_common_poll_internet_reachability(void);

void inet_common_mdns_init(void);
void inet_common_mdns_shutdown(void);

void inet_common_sntp_init(void);
static inline void inet_common_sntp_shutdown(void) {
    extern void esp_netif_sntp_deinit(void);
    esp_netif_sntp_deinit();
}

void inet_common_mqtt_init(void);
static inline void inet_common_mqtt_shutdown(void) {
    extern void mqtt_shutdown(void);
    mqtt_shutdown();
}

#ifdef __cplusplus
}
#endif
