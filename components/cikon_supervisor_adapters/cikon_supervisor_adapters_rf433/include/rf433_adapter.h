#pragma once

#include <stdint.h>

#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RF433 event callback type
 *
 * @param code Received RF code
 * @param bits Number of bits received
 */
typedef void (*rf433_event_callback_t)(uint32_t code, uint8_t bits);

/**
 * @brief Register custom callback for RF433 events
 *
 * When registered, this callback will be called for all received codes,
 * overriding the default behavior. If NULL, default actions are used.
 *
 * @param callback Function to call on RF433 events, or NULL to use defaults
 */
void rf433_adapter_register_callback(rf433_event_callback_t callback);

/**
 * @brief RF433 adapter instance
 *
 * Provides 433MHz RF receiver functionality
 * with Home Assistant integration.
 */
extern supervisor_platform_adapter_t rf433_adapter;

#ifdef __cplusplus
}
#endif
