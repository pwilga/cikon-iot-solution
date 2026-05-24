#pragma once

#include "esp_err.h"
#include "esp_eth.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Ethernet hardware (auto-selects backend based on Kconfig)
 * 
 * Initializes the appropriate Ethernet backend:
 * - W5500: SPI bus + MAC + PHY + driver install
 * - OpenETH (ESP32-P4): Internal MAC + external PHY + driver install
 * 
 * @param[out] out_handle Ethernet driver handle (for esp_netif attachment)
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if already initialized
 *      - ESP_ERR_NO_MEM if memory allocation failed
 *      - ESP_FAIL on any other error
 */
esp_err_t ethernet_init(esp_eth_handle_t *out_handle);

/**
 * @brief Shutdown Ethernet hardware
 * 
 * Stops Ethernet driver, frees MAC/PHY instances, deinitializes hardware.
 * 
 * @param[in] handle Ethernet driver handle from ethernet_init()
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 *      - ESP_ERR_INVALID_ARG if handle is NULL
 */
esp_err_t ethernet_shutdown(esp_eth_handle_t handle);

/**
 * @brief Get backend name (for debugging)
 * 
 * @return Constant string with backend name ("W5500", "OpenETH+IP101", etc.)
 */
const char *ethernet_get_backend_name(void);

#ifdef __cplusplus
}
#endif
