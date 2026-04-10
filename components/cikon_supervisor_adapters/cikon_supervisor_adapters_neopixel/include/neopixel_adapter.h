#pragma once

#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief NeoPixel adapter instance for supervisor
 *
 * Provides WS2812 LED strip control via cmnd/tele interface.
 *
 * Commands:
 *   neopixel {"color":"#RRGGBB"}               - set solid color
 *   neopixel {"effect":"rainbow","speed":5}    - start effect
 *   neopixel {"effect":"none"}                 - stop effect
 *   neopixel {"brightness":128}                - set brightness (0-255)
 *
 * {"neopixel":{"effect":"rainbow","speed":10,"brightness":55}}
 * {"neopixel":{"color":"#0f4aca","brightness":10}}
 *
 * Telemetry:
 *   neopixel_state: current effect name and brightness
 */
extern supervisor_platform_adapter_t neopixel_adapter;

#ifdef __cplusplus
}
#endif
