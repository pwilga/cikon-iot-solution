#ifndef CMND_CORE_HANDLERS_H
#define CMND_CORE_HANDLERS_H

/**
 * @brief Register core command handlers
 * 
 * Registers platform-independent commands:
 * - restart: Restart the device
 * - help: Show available commands
 * - log: Print debug information
 * - setconf: Set configuration from JSON
 * - resetconf: Reset configuration and restart
 * - onboard_led: Set onboard LED state
 */
void cmnd_core_handlers_register(void);

#endif // CMND_CORE_HANDLERS_H
