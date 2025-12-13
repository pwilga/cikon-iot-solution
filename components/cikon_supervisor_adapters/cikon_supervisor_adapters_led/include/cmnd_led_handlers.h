#pragma once

/**
 * @brief Register LED adapter command handlers
 *
 * Registers LED-specific commands:
 * - led: Set LED brightness (0-255)
 */
void led_cmnd_handlers_register(void);

/**
 * @brief Unregister LED adapter command handlers
 *
 * Removes all LED-specific commands from the registry
 */
void led_cmnd_handlers_unregister(void);
