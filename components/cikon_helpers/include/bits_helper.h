#pragma once

/**
 * @file bits_helper.h
 * @brief Event Group Bit Allocation Registry for Cikon Platform
 *
 * This file defines ALL event group bits used across the supervisor.
 * Centralizes bit allocation to prevent conflicts and enable cross-adapter
 * communication (e.g., adapter A checking if adapter B is ready).
 *
 * Allocation Strategy:
 * - All bits defined HERE for cross-adapter visibility
 * - Allocate sequentially (next available bit)
 * - Sort by bit number (BIT0, BIT1, BIT2...)
 * - Document adapter name + purpose with each bit
 */

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/event_groups.h"

#define SUPERVISOR_EVENT_CMND_COMPLETED BIT0
#define SUPERVISOR_EVENT_PLATFORM_INITIALIZED BIT1

// BIT2-6: Available

#define NETWORK_AP_READY BIT7       // Network: WiFi access point started
#define NETWORK_INTERNET_READY BIT8 // Network: internet reachable (TCP to 8.8.8.8:53)
#define NETWORK_INTERNET_LOST BIT9  // Network: internet no longer reachable

// BIT10-11: Available

#define THREAD_BORDER_ROUTER_READY BIT12 // Thread border router initialized

// BIT13: Available

#define NETWORK_UPLINK_READY BIT14 // Network: active uplink has IP into the LAN (any uplink)
#define NETWORK_UPLINK_LOST BIT15  // Network: active uplink lost

// BIT16-23: Available (8 bits)
