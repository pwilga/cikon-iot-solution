#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool is_tcp_port_reachable(const char *host, uint16_t port);

bool is_internet_reachable(void);
bool is_mqtt_broker_reachable(void);

/**
 * @brief Checks if any local network interface is connected (e.g. Wi-Fi, Ethernet).
 *
 * This function verifies local network connectivity, regardless of interface type.
 * It does NOT guarantee internet access, only that a network link is up.
 *
 * For checking internet reachability, use @ref is_internet_reachable.
 *
 * @return true if any network interface is connected, false otherwise.
 */
// bool is_network_connected(void);

#ifdef __cplusplus
}
#endif

#endif // NET_H
