#pragma once

#include "cJSON.h"

/**
 * @brief Copy the Thread network state from OpenThread for thread_tele_appender().
 *
 * Always call it from the same task. Does nothing until the OpenThread stack has started.
 */
void thread_tele_refresh(void);

/**
 * @brief Tele appender of the Thread network state.
 *
 * Adds the object @p tele_id with the role, network name, channel, TX power, PAN IDs, partition ID,
 * EUI-64 and the addresses with their types. The border agent ID, OMR prefix and the devices
 * registered on the SRP server come only with a build that has them, the commissioner state only
 * with one that has the commissioner. Adds nothing before the first thread_tele_refresh().
 */
void thread_tele_appender(const char *tele_id, cJSON *json_root);

/**
 * @brief Tele appender of the address this device is reachable at: the OMR address, or the
 * mesh-local EID when there is none.
 */
void thread_tele_ip_appender(const char *tele_id, cJSON *json_root);

/** @brief Log the Thread network state. */
void thread_tele_log_network(void);
