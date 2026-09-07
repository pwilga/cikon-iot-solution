#pragma once

// Color math for the light adapter: the pipeline that turns a picked color plus a brightness
// into what the hardware is driven with. light.c and light_effects.c both build colors and both
// hand them to light_color_to_levels(); nothing else applies gamma or brightness anywhere.
//
// Deliberately free of ESP-IDF headers - only <stdint.h> here and <math.h> in the .c - so it
// compiles and runs on a host, and a change to the curve can be checked numerically without a
// board.

#include <stdint.h>

// One light's five channels: r/g/b plus the cold/warm whites. The same struct carries a color
// in and finished output levels back out, so there is only one shape to think about - what
// changes between the two is the meaning, not the range:
//
//   in  - plain sRGB, the color as it was picked, before gamma and before dimming
//   out - drive levels, gamma-corrected and dimmed, ready for the hardware
//
// Eight bits per channel throughout, which is what a WS2812 clocks out and what the LEDC duty
// is configured for.
typedef struct {
    uint8_t r, g, b;
    uint8_t c, w;
} light_rgbcw_t;

// Builds the gamma table. Call once, before anything else here.
void light_color_init(void);

// Standard HSV -> RGB. hue is degrees (0-359), saturation and value are percentages (0-100).
// The render path always passes value = 100 and leaves dimming to light_color_to_levels(); value
// is there for tele, which reports a color for a UI to draw rather than a drive level.
void light_color_hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value, uint8_t *red,
                            uint8_t *green, uint8_t *blue);

// The output stage, and the only place gamma or brightness is applied: gamma-corrects the
// color, then dims it linearly by brightness (0-100). What comes back goes straight into
// led_strip or an LEDC duty.
light_rgbcw_t light_color_to_levels(light_rgbcw_t color, uint8_t brightness);
