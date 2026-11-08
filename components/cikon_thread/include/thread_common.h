#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "openthread/dataset.h"

#include "cmnd.h"

bool thread_dataset_parse_hex(const char *hex, otOperationalDatasetTlvs *out);

#if CONFIG_OPENTHREAD_SRP_CLIENT
/**
 * @brief Starts the SRP client under @p hostname: the border router announces the name on the LAN.
 *
 * @p hostname has to stay valid, the client callback logs it.
 */
void thread_srp_host_init(const char *hostname);

/**
 * @brief Registers a service of this host with the border router, e.g. "_http._tcp".
 *
 * One service at a time - a second call replaces it.
 */
void thread_srp_add_service(const char *instance, const char *type, uint16_t port);
#endif

#if CONFIG_OPENTHREAD_DNS64_CLIENT
#include "lwip/ip6_addr.h"

/** @brief Size of the buffer thread_nat64_address() needs. */
#define THREAD_NAT64_ADDRESS_MAX IP6ADDR_STRLEN_MAX

/**
 * @brief Address of an IPv4 host as seen from Thread: the border router's NAT64 prefix with the
 * IPv4 address glued on.
 *
 * @return false when the border router has not advertised a prefix yet
 */
bool thread_nat64_address(const char *ipv4, char *buf, size_t buflen);
#endif

/**
 * @brief Supervisor commands of the Thread services built into this device.
 *
 * Each service is there only when its OpenThread option is on; without any the array holds just
 * the terminator.
 */
extern const command_entry_t thread_commands[];
