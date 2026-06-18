#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *app_name;
    const char *app_version;
    const char *idf_version;
    const char *chip;
    uint32_t chip_rev;
    uint32_t cores;
    char id[13]; // MAC from eFuse, e.g. "A1B2C3D4E5F6"
} device_info_t;

const device_info_t *get_device_info(void);

void core_system_init(void);

void set_restart_callback(void (*cb)(void));
void esp_safe_restart();

/**
 * @brief Initializes the NVS (Non-Volatile Storage) flash partition.
 *
 * This function ensures that the NVS is properly initialized, even in cases
 * where the flash partition has run out of free pages or a newer NVS version
 * is detected (e.g. after firmware upgrade or flash format).
 *
 * It should be called once during system startup (before using any NVS or Wi-Fi
 * functionality).
 *
 * **Required for Wi-Fi to function**: The ESP-IDF Wi-Fi stack stores
 * configuration and calibration data in NVS. Skipping this initialization will
 * result in Wi-Fi startup failure.
 */
esp_err_t nvs_flash_safe_init();


/**
 * @brief Returns the ISO8601-formatted boot time of the system.
 *
 * Calculates the startup timestamp once and returns the cached value
 * on subsequent calls.
 *
 * @return Pointer to a static ISO8601 string (UTC).
 */
const char *get_boot_time(void);

void onboard_led_set_state(bool state);
bool get_onboard_led_state(void);

/**
 * @brief Erase and re-initialize the entire NVS partition (factory reset of all non-volatile
 * storage). After this call, NVS is ready for use.
 */
void reset_nvs_partition(void);

#ifdef __cplusplus
}
#endif
