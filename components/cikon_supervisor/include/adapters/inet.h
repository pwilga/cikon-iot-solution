#ifndef SUPERVISOR_ADAPTER_INET_H
#define SUPERVISOR_ADAPTER_INET_H

#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Internet platform adapter instance
 *
 * Provides WiFi/Ethernet + MQTT + HTTPS + SNTP + OTA support for supervisor.
 * Handles network initialization, time synchronization, and protocol services.
 */
extern supervisor_platform_adapter_t inet_adapter;

/**
 * @brief Switch WiFi to AP mode
 *
 * Performs graceful shutdown of all services (MQTT, HTTPS, mDNS, SNTP),
 * then switches WiFi to Access Point mode. Services appropriate for AP
 * mode (HTTPS, mDNS) will be automatically restarted by event handlers.
 */
void inet_switch_to_ap_mode(void);

/**
 * @brief Switch WiFi to STA mode
 *
 * Performs graceful shutdown of all services (MQTT, HTTPS, mDNS, SNTP),
 * then switches WiFi to Station mode. Services appropriate for STA mode
 * (MQTT, SNTP, mDNS) will be automatically restarted by event handlers.
 */
void inet_switch_to_sta_mode(void);

#ifdef __cplusplus
}
#endif

#endif // SUPERVISOR_ADAPTER_INET_H
