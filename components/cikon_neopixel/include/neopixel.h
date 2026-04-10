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
 * @return ESP_OK on success
 */
esp_err_t neopixel_init(int gpio, uint16_t count);

/**
 * @brief Deinitialize neopixel strip and free resources
 */
esp_err_t neopixel_deinit(void);

/**
 * @brief Set pixel color by linear index
 */
esp_err_t neopixel_set_pixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b);

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
