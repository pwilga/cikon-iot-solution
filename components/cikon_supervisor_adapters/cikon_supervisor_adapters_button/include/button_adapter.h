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
 * @param button_name Name of the button (from Kconfig, or auto-generated "gpio<N>" if unnamed) -
 *                    same value as button_adapter_get_name(button_idx), passed here for
 *                    convenience so callbacks don't have to look it up themselves
 * @param event Button event type (BUTTON_SINGLE_CLICK, BUTTON_DOUBLE_CLICK, etc.)
 */
typedef void (*button_event_callback_t)(uint8_t button_idx, const char *button_name,
                                        button_event_t event);

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
 * @brief Get the human-readable name of a configured button
 *
 * @param idx Button index (0-based)
 * @return Name (from Kconfig, or auto-generated "gpio<N>" if unnamed),
 *         or NULL if idx is out of range for the number of configured buttons
 */
const char *button_adapter_get_name(uint8_t idx);

/**
 * @brief Log a button event at INFO level, using the button's name (see button_adapter_get_name)
 *
 * Convenience helper for simple device_handlers.c callbacks that just want readable
 * logging without reimplementing the same switch/name lookup per device.
 *
 * @param button_idx Button index (0-based)
 * @param event Button event type
 */
void button_adapter_log_event(uint8_t button_idx, button_event_t event);

/**
 * @brief Button adapter for supervisor
 *
 * Provides physical button input handling with configurable actions
 */
extern supervisor_platform_adapter_t button_adapter;

#ifdef __cplusplus
}
#endif
