#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NEOPIXEL_EFFECT_NONE = 0,
    NEOPIXEL_EFFECT_SOLID,
    NEOPIXEL_EFFECT_BLINK,
    NEOPIXEL_EFFECT_PULSE,
    NEOPIXEL_EFFECT_RAINBOW,
    NEOPIXEL_EFFECT_SCAN,
    NEOPIXEL_EFFECT_MATRIX,
} neopixel_effect_t;

/**
 * @brief Start a visual effect
 *
 * Stops any currently running effect and starts the new one in a FreeRTOS task.
 *
 * @param effect  Effect type
 * @param color   RGB color packed as 0x00RRGGBB (ignored for rainbow/matrix)
 * @param speed   Effect speed (1=slow, 10=fast)
 */
void neopixel_effect_start(neopixel_effect_t effect, uint32_t color, uint8_t speed);

/**
 * @brief Stop the currently running effect and clear the strip
 */
void neopixel_effect_stop(void);

/**
 * @brief Get the currently active effect
 */
neopixel_effect_t neopixel_effect_get_current(void);

#ifdef __cplusplus
}
#endif
