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
 * @brief Add the commissioner state to the "thread" tele object.
 *
 * Adds "commissioner": {"state": "disabled"|"petition"|"active", "remaining": seconds left, only
 * when active}. Does nothing when OpenThread has no commissioner.
 *
 * @param thread  The "thread" object of the tele.
 */
#if CONFIG_OPENTHREAD_COMMISSIONER
void thread_commissioner_tele(cJSON *thread);
#else
static inline void thread_commissioner_tele(cJSON *thread) { (void)thread; }
#endif

#if CONFIG_OPENTHREAD_CLI
void thread_console_start(const char *prompt);
void thread_cli_commands_init(void);
#endif
