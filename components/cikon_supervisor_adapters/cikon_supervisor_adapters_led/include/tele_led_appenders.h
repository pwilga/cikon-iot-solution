#pragma once

/**
 * @brief Register LED adapter telemetry appenders
 *
 * Registers LED-specific telemetry data:
 * - led: Current LED brightness (0-255)
 */
void led_tele_appenders_register(void);

/**
 * @brief Unregister LED adapter telemetry appenders
 *
 * Removes all LED-specific telemetry appenders from the registry
 */
// void led_tele_appenders_unregister(void);
