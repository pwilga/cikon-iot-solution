#pragma once

#include "esp_err.h"
#include "esp_eth.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Ethernet stack (hardware + netif + glue + start driver)
 *
 * Initializes:
 * - TCP/IP stack (esp_netif_init)
 * - Ethernet backend (W5500 SPI or OpenETH)
 * - esp_netif for Ethernet
 * - Glue layer (connects driver to TCP/IP stack)
 * - Attaches netif to driver
 * - Starts driver (link negotiation)
 *
 * Pattern matches all adapters - init does EVERYTHING.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if already initialized
 *      - ESP_ERR_NO_MEM if memory allocation failed
 *      - ESP_FAIL on any other error
 */
esp_err_t ethernet_init(void);

/**
 * @brief Shutdown Ethernet stack completely
 *
 * Stops driver, detaches netif, frees glue, destroys netif, deinitializes hardware.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t ethernet_shutdown(void);

/**
 * @brief Get backend name (for debugging)
 *
 * @return Constant string with backend name ("W5500", "OpenETH+IP101", etc.)
 */
const char *ethernet_get_backend_name(void);

/**
 * @brief Get Ethernet interface IP address
 *
 * @param[out] buf Buffer for IP string (e.g. "192.168.1.100")
 * @param[in] buflen Buffer size
 */
void ethernet_get_interface_ip(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif
