#pragma once

#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Network platform adapter instance
 *
 * Brings the device onto the network over one uplink (WiFi) and runs the services that depend
 * on it: MQTT, HTTP, SNTP, mDNS, TCP OTA and TCP monitor.
 */
extern supervisor_platform_adapter_t network_adapter;

#ifdef __cplusplus
}
#endif
