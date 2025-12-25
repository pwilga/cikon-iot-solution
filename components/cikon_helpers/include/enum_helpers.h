#pragma once

#include "esp_flash_partitions.h"
#include "esp_system.h"

/**
 * @brief Convert ESP OTA image state enum to string representation
 * @param state OTA image state enum value
 * @return String representation of the state (e.g., "ESP_OTA_IMG_VALID")
 */
static inline const char *esp_ota_state_to_string(esp_ota_img_states_t state) {
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "ESP_OTA_IMG_NEW";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "ESP_OTA_IMG_PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:
        return "ESP_OTA_IMG_VALID";
    case ESP_OTA_IMG_INVALID:
        return "ESP_OTA_IMG_INVALID";
    case ESP_OTA_IMG_ABORTED:
        return "ESP_OTA_IMG_ABORTED";
    case ESP_OTA_IMG_UNDEFINED:
        return "ESP_OTA_IMG_UNDEFINED";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief Convert ESP reset reason enum to string representation
 * @param reason Reset reason enum value
 * @return String representation of the reason (e.g., "ESP_RST_POWERON")
 */
static inline const char *esp_reset_reason_to_string(esp_reset_reason_t reason) {
    switch (reason) {
    case ESP_RST_UNKNOWN:
        return "ESP_RST_UNKNOWN";
    case ESP_RST_POWERON:
        return "ESP_RST_POWERON";
    case ESP_RST_EXT:
        return "ESP_RST_EXT";
    case ESP_RST_SW:
        return "ESP_RST_SW";
    case ESP_RST_PANIC:
        return "ESP_RST_PANIC";
    case ESP_RST_INT_WDT:
        return "ESP_RST_INT_WDT";
    case ESP_RST_TASK_WDT:
        return "ESP_RST_TASK_WDT";
    case ESP_RST_WDT:
        return "ESP_RST_WDT";
    case ESP_RST_DEEPSLEEP:
        return "ESP_RST_DEEPSLEEP";
    case ESP_RST_BROWNOUT:
        return "ESP_RST_BROWNOUT";
    case ESP_RST_SDIO:
        return "ESP_RST_SDIO";
    case ESP_RST_USB:
        return "ESP_RST_USB";
    case ESP_RST_JTAG:
        return "ESP_RST_JTAG";
    case ESP_RST_EFUSE:
        return "ESP_RST_EFUSE";
    case ESP_RST_PWR_GLITCH:
        return "ESP_RST_PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP:
        return "ESP_RST_CPU_LOCKUP";
    default:
        return "ESP_RST_UNKNOWN";
    }
}

/**
 * @brief Check if reset reason indicates an abnormal reset (panic, watchdog, etc.)
 * @param reason Reset reason enum value
 * @return true if abnormal reset, false otherwise
 */
static inline bool is_abnormal_reset(esp_reset_reason_t reason) {
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}
