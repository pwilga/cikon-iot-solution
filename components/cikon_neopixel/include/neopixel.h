#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize neopixel strip
 *
 * @param gpio  GPIO pin connected to data line
 * @param count Number of LEDs in the strip
 * @param width Matrix width (1 for linear strip, e.g. 8 for 8x8 matrix)
 * @return ESP_OK on success
 */
esp_err_t neopixel_init(int gpio, uint16_t count, uint8_t width);

/**
 * @brief Deinitialize neopixel strip and free resources
 */
esp_err_t neopixel_deinit(void);

/**
 * @brief Set pixel color by linear index
 */
esp_err_t neopixel_set_pixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Set pixel color by 2D coordinates (for matrix)
 *
 * Uses snake/zigzag mapping if enabled in Kconfig.
 */
esp_err_t neopixel_set_xy(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Fill all pixels with a single color
 */
esp_err_t neopixel_fill(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Clear all pixels (set to black)
 */
esp_err_t neopixel_clear(void);

/**
 * @brief Push pixel data to strip (must be called after set_pixel/fill)
 */
esp_err_t neopixel_show(void);

/**
 * @brief Get number of LEDs
 */
uint16_t neopixel_get_count(void);

/**
 * @brief Get matrix width (1 for linear)
 */
uint8_t neopixel_get_width(void);

/**
 * @brief Set global brightness (0-255), applied on next show()
 */
void neopixel_set_brightness(uint8_t brightness);

/**
 * @brief Get current brightness
 */
uint8_t neopixel_get_brightness(void);

#ifdef __cplusplus
}
#endif
