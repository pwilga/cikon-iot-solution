#pragma once

/**
 * @brief Register inet adapter command handlers
 *
 * Registers internet/network-specific commands:
 * - ap: Switch to AP mode
 * - sta: Switch to STA mode
 * - https: Control HTTPS server
 * - sntp: Control SNTP service
 * - ha: Trigger Home Assistant MQTT discovery
 */
void inet_cmnd_handlers_register(void);

/**
 * @brief Unregister inet adapter command handlers
 *
 * Removes all internet/network-specific commands from the registry
 */
void inet_cmnd_handlers_unregister(void);
