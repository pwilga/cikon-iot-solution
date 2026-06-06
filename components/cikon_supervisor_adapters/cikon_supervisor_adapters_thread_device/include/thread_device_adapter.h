#pragma once

#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Thread Device platform adapter instance
 *
 * Joins an existing Thread network as FTD (router-capable) or MTD/SED (sleepy end device).
 * Uses the native 802.15.4 radio — targets ESP32-H2 and ESP32-C6.
 * No backbone (Ethernet/WiFi) required — starts immediately after boot.
 *
 * Dataset priority: CONFIG_THREAD_DEVICE_PROVISIONED_DATASET → NVS → unconfigured (CLI needed).
 */
extern supervisor_platform_adapter_t thread_device_adapter;

#ifdef __cplusplus
}
#endif
