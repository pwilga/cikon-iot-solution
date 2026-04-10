#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Predefined RGB colors (24-bit: 0x00RRGGBB)
 */
typedef enum {
    // Primary colors
    NEOPIXEL_COLOR_BLACK = 0x000000,
    NEOPIXEL_COLOR_WHITE = 0xFFFFFF,
    NEOPIXEL_COLOR_RED = 0xFF0000,
    NEOPIXEL_COLOR_GREEN = 0x00FF00,
    NEOPIXEL_COLOR_BLUE = 0x0000FF,

    // Secondary colors
    NEOPIXEL_COLOR_YELLOW = 0xFFFF00,
    NEOPIXEL_COLOR_CYAN = 0x00FFFF,
    NEOPIXEL_COLOR_MAGENTA = 0xFF00FF,

    // Common colors
    NEOPIXEL_COLOR_ORANGE = 0xFF8000,
    NEOPIXEL_COLOR_PURPLE = 0x800080,
    NEOPIXEL_COLOR_PINK = 0xFF69B4,
    NEOPIXEL_COLOR_LIME = 0x00FF00,

    // Warm whites
    NEOPIXEL_COLOR_WARM_WHITE = 0xFDF4DC,
    NEOPIXEL_COLOR_COOL_WHITE = 0xF0F8FF,
} neopixel_color_t;

/**
 * @brief Create RGB color from components
 */
static inline uint32_t neopixel_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/**
 * @brief Create HSV color (hue 0-360, sat 0-100, val 0-100)
 */
uint32_t neopixel_hsv(uint16_t hue, uint8_t sat, uint8_t val);

#ifdef __cplusplus
}
#endif
