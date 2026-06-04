#pragma once

#include "esp_err.h"
#include "esp_eth_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Ethernet backend interface
 *
 * Each backend (W5500, OpenETH, etc.) implements this interface.
 * Main wrapper (ethernet.c) selects backend at compile-time based on Kconfig.
 */
typedef struct {
    /**
     * @brief Initialize hardware and create Ethernet driver
     *
     * @param[out] out_handle Ethernet driver handle
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t (*init)(esp_eth_handle_t *out_handle);

    /**
     * @brief Shutdown hardware and destroy Ethernet driver
     *
     * @param[in] handle Ethernet driver handle
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t (*shutdown)(esp_eth_handle_t handle);

    /**
     * @brief Backend name (for logging/debugging)
     */
    const char *name;
} ethernet_backend_t;

// Backend instances (defined in backend .c files)
#ifdef CONFIG_ETHERNET_W5500
extern const ethernet_backend_t ethernet_backend_w5500;
#endif

#ifdef CONFIG_ETHERNET_OPENETH
extern const ethernet_backend_t ethernet_backend_openeth;
#endif

#ifdef __cplusplus
}
#endif
