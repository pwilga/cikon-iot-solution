#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "openthread/dataset.h"

#include "cJSON.h"
#include "cmnd.h"

bool thread_dataset_parse_hex(const char *hex, otOperationalDatasetTlvs *out);
void thread_log_network_info(void);

/**
 * @brief Get the most widely reachable IPv6 address of this Thread device.
 *
 * Prefers an OMR (Off-Mesh Routable) address — the only Thread address type
 * routed by the Border Router onto the backbone network, so it's reachable
 * from the whole LAN/WLAN (and the internet if a GUA prefix is delegated).
 * Falls back to the Mesh-Local EID if no OMR address is available yet (e.g.
 * right after joining, before the Border Router advertises one). Never
 * returns a link-local or RLOC/ALOC address.
 *
 * @param out       Buffer to receive the address string (use OT_IP6_ADDRESS_STRING_SIZE).
 * @param out_size  Size of @p out.
 * @return true if a reachable address was found and written to @p out.
 */
bool thread_get_reachable_ip6(char *out, size_t out_size);

/**
 * @brief Supervisor commands of the Thread services built into this device.
 *
 * Each service is there only when its OpenThread option is on; without any the array holds just
 * the terminator.
 */
extern const command_entry_t thread_commands[];

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

#if CONFIG_OPENTHREAD_CLI
void thread_console_start(const char *prompt);
void thread_cli_commands_init(void);
#endif
