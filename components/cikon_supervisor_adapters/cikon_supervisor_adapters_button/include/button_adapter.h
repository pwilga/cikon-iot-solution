#pragma once

#include "iot_button.h"
#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Button event callback type
 *
 * @param button_idx Button index (0-based)
 * @param event Button event type (BUTTON_SINGLE_CLICK, BUTTON_DOUBLE_CLICK, etc.)
 */
typedef void (*button_event_callback_t)(uint8_t button_idx, button_event_t event);

/**
 * @brief Register custom callback for button events
 *
 * When registered, this callback will be called for all button events,
 * overriding the default behavior. If NULL, default actions are used.
 *
 * @param callback Function to call on button events, or NULL to use defaults
 */
void button_adapter_register_callback(button_event_callback_t callback);

/**
 * @brief Button adapter for supervisor
 *
 * Provides physical button input handling with configurable actions
 */
extern supervisor_platform_adapter_t button_adapter;

#ifdef __cplusplus
}
#endif
