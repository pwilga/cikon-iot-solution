#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *app_version;
    const char *app_build_time; // ISO8601 UTC; corrected from the build host's local
                                // clock via CIKON_BUILD_TZ_OFFSET_S (see platform_services.c)
    const char *idf_version;
    const char *chip;
    uint32_t chip_rev;
    uint32_t cores;
    char id[13]; // MAC from eFuse, e.g. "A1B2C3D4E5F6"
    uint32_t bootloader_version;
    const char *bootloader_idf_version;
    const char *bootloader_build_time; // ISO8601 UTC; corrected the same way as app_build_time
} device_info_t;

const device_info_t *get_device_info(void);

void core_system_init(void);

void set_restart_callback(void (*cb)(void));
void esp_safe_restart();
/**
 * @brief Returns true from the moment esp_safe_restart() is called until the device resets.
 *
 * Use this to skip teardown steps that are irrelevant during restart (e.g. OTA shutdown),
 * avoiding the need for per-adapter boolean flags that are easy to forget.
 */
bool restart_pending(void);

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

const char **get_chip_features(void);
uint32_t get_flash_size(void);
size_t get_psram_size(void);
int get_cpu_freq_mhz(void);
bool get_fs_info(size_t *used, size_t *total);
bool get_chip_temp(float *out);

#ifdef __cplusplus
}
#endif
