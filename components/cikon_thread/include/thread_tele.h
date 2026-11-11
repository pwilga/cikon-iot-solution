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
 *
 * With THREAD_TELE_COUNTERS on, the health of the link and of the stack is in the objects `mac`
 * (frames sent and received, retries and failed channel checks), `mle` (role and parent changes),
 * `ip` (packets), `buffers` (the message buffer pool: total, free and the most taken at once) and
 * the array `neighbors` (RSSI and link quality of every neighbour). A border router adds `br`
 * (traffic between Thread and the LAN, router advertisements, routers on the LAN, SRP server
 * state). All counters count from the start of the device; only `mac.cca_fail_pct` is of the latest
 * frames.
 */
void thread_tele_appender(const char *tele_id, cJSON *json_root);

/**
 * @brief Tele appender of the address this device is reachable at: the OMR address, or the
 * mesh-local EID when there is none.
 */
void thread_tele_ip_appender(const char *tele_id, cJSON *json_root);

/** @brief Log the Thread network state. */
void thread_tele_log_network(void);
