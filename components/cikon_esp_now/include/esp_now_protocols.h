#pragma once

#include <stdint.h>

/**
 * @brief Wizmote button protocol structure (13 bytes)
 *
 * Used by WiZ/Philips smart lighting remote controls.
 * This is a de-facto standard for ESP-NOW remotes.
 */
typedef struct {
    uint8_t program;       // 0x91 for ON button, 0x81 for all others
    uint8_t seq[4];        // 32-bit sequence number (LSB first)
    uint8_t dt1;           // Button data type (0x32)
    uint8_t button;        // Button code (1-255)
    uint8_t dt2;           // Battery data type (0x01)
    uint8_t battery_level; // Battery level (0-100)
    uint8_t checksum[4];   // Checksums (4 bytes)
} wizmote_message_t;       // Total: 13 bytes

/**
 * @brief Wizmote button codes
 */
typedef enum {
    WIZMOTE_BUTTON_ON = 1,
    WIZMOTE_BUTTON_OFF = 2,
    WIZMOTE_BUTTON_NIGHT = 3,
    WIZMOTE_BUTTON_BRIGHT_DOWN = 8,
    WIZMOTE_BUTTON_BRIGHT_UP = 9,
    WIZMOTE_BUTTON_ONE = 16,
    WIZMOTE_BUTTON_TWO = 17,
    WIZMOTE_BUTTON_THREE = 18,
    WIZMOTE_BUTTON_FOUR = 19,
} wizmote_button_t;

/**
 * @brief Get human-readable name for Wizmote button code
 * @param button_id Button code from wizmote_message_t
 * @return String name of the button
 */
static inline const char *wizmote_button_name(uint8_t button_id) {
    switch (button_id) {
    case WIZMOTE_BUTTON_ON:
        return "WIZMOTE_BUTTON_ON";
    case WIZMOTE_BUTTON_OFF:
        return "WIZMOTE_BUTTON_OFF";
    case WIZMOTE_BUTTON_NIGHT:
        return "WIZMOTE_BUTTON_NIGHT";
    case WIZMOTE_BUTTON_BRIGHT_DOWN:
        return "WIZMOTE_BUTTON_BRIGHT_DOWN";
    case WIZMOTE_BUTTON_BRIGHT_UP:
        return "WIZMOTE_BUTTON_BRIGHT_UP";
    case WIZMOTE_BUTTON_ONE:
        return "WIZMOTE_BUTTON_ONE";
    case WIZMOTE_BUTTON_TWO:
        return "WIZMOTE_BUTTON_TWO";
    case WIZMOTE_BUTTON_THREE:
        return "WIZMOTE_BUTTON_THREE";
    case WIZMOTE_BUTTON_FOUR:
        return "WIZMOTE_BUTTON_FOUR";
    default:
        return "WIZMOTE_BUTTON_UNKNOWN";
    }
}
