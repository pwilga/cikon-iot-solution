#pragma once

#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief INET adapter instance
 *
 * Provides WiFi, MQTT, Home Assistant, HTTP server,
 * TCP OTA, and TCP monitoring functionality.
 */
extern supervisor_platform_adapter_t inet_adapter;

#ifdef __cplusplus
}
#endif
