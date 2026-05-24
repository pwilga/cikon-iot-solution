#pragma once

#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Internet Ethernet platform adapter instance
 *
 * Provides Ethernet + MQTT + mDNS + SNTP + OTA support for supervisor.
 * Handles network initialization, time synchronization, and protocol services.
 * 
 * Uses cikon_ethernet (hardware layer) for W5500/ESP32-P4 OpenETH support.
 */
extern supervisor_platform_adapter_t inet_ethernet_adapter;

#ifdef __cplusplus
}
#endif
