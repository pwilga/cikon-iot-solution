#pragma once

#include "esp_chip_info.h"
#include "esp_flash_partitions.h"
#include "esp_system.h"

/**
 * @brief Convert ESP OTA image state enum to a human-readable string
 * @param state OTA image state enum value
 * @return Human-readable description of the state (e.g., "Valid")
 */
static inline const char *esp_ota_state_to_string(esp_ota_img_states_t state) {
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "New";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "Pending verify";
    case ESP_OTA_IMG_VALID:
        return "Valid";
    case ESP_OTA_IMG_INVALID:
        return "Invalid";
    case ESP_OTA_IMG_ABORTED:
        return "Aborted";
    case ESP_OTA_IMG_UNDEFINED:
        return "Undefined";
    default:
        return "Unknown";
    }
}

/**
 * @brief Convert ESP reset reason enum to a human-readable string
 * @param reason Reset reason enum value
 * @return Human-readable description of the reason (e.g., "Power on")
 */
static inline const char *esp_reset_reason_to_string(esp_reset_reason_t reason) {
    switch (reason) {
    case ESP_RST_UNKNOWN:
        return "Unknown";
    case ESP_RST_POWERON:
        return "Power on";
    case ESP_RST_EXT:
        return "External pin";
    case ESP_RST_SW:
        return "Software";
    case ESP_RST_PANIC:
        return "Panic";
    case ESP_RST_INT_WDT:
        return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:
        return "Task watchdog";
    case ESP_RST_WDT:
        return "Other watchdog";
    case ESP_RST_DEEPSLEEP:
        return "Deep sleep wake";
    case ESP_RST_BROWNOUT:
        return "Brownout";
    case ESP_RST_SDIO:
        return "SDIO";
    case ESP_RST_USB:
        return "USB peripheral";
    case ESP_RST_JTAG:
        return "JTAG";
    case ESP_RST_EFUSE:
        return "Efuse error";
    case ESP_RST_PWR_GLITCH:
        return "Power glitch";
    case ESP_RST_CPU_LOCKUP:
        return "CPU lockup";
    default:
        return "Unknown";
    }
}

/**
 * @brief Check if reset reason indicates an abnormal reset (panic, watchdog, etc.)
 * @param reason Reset reason enum value
 * @return true if abnormal reset, false otherwise
 */
static inline bool is_abnormal_reset(esp_reset_reason_t reason) {
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT;
}

/**
 * @brief Convert ESP chip model enum to string representation
 * @param model Chip model enum value
 * @return String representation of the model (e.g., "esp32s3")
 */
static inline const char *esp_chip_model_to_string(esp_chip_model_t model) {
    switch (model) {
    case CHIP_ESP32:
        return "esp32";
    case CHIP_ESP32S2:
        return "esp32s2";
    case CHIP_ESP32S3:
        return "esp32s3";
    case CHIP_ESP32C3:
        return "esp32c3";
    case CHIP_ESP32C2:
        return "esp32c2";
    case CHIP_ESP32C6:
        return "esp32c6";
    case CHIP_ESP32H2:
        return "esp32h2";
    case CHIP_ESP32P4:
        return "esp32p4";
    case CHIP_ESP32C61:
        return "esp32c61";
    case CHIP_ESP32C5:
        return "esp32c5";
    case CHIP_ESP32H21:
        return "esp32h21";
    case CHIP_ESP32H4:
        return "esp32h4";
    case CHIP_POSIX_LINUX:
        return "posix_linux";
    default:
        return "unknown";
    }
}
