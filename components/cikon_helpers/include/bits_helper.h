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

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define SUPERVISOR_EVENT_CMND_COMPLETED  BIT0
#define SUPERVISOR_EVENT_PLATFORM_INITIALIZED  BIT1

// BIT2-3: Available

#define INET_EVENT_TIME_SYNCED  BIT4
#define INET_EVENT_STA_READY  BIT5
#define INET_EVENT_AP_READY  BIT6

// BIT7: Available

#define ESPNOW_EVENT_NETWORK_READY  BIT8

// BIT9-23: Available (15 bits)
