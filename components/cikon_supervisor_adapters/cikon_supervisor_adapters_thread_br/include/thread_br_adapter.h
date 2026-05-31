#pragma once

#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Thread Border Router platform adapter instance
 *
 * Bridges Thread (802.15.4) network to backbone (Ethernet or WiFi).
 * Starts OpenThread stack after any backbone IP is available (INET_ETH_READY or INET_EVENT_STA_READY).
 * RCP connected via UART to external ESP32-H2/C6.
 */
extern supervisor_platform_adapter_t thread_br_adapter;

#ifdef __cplusplus
}
#endif
