#pragma once

// Color math for the light driver: what turns a picked color plus a brightness into the numbers
// the hardware is driven with.
//
// Deliberately free of ESP-IDF headers - only <stdint.h> here and <math.h> in the .c - so it
// compiles and runs on a host, and a change to a curve can be checked numerically without a
// board.

#include <stdint.h>

// A color as it was picked: plain sRGB, before gamma and before dimming. r/g/b carry the color,
// c/w the cold and warm white channels.
typedef struct {
    uint8_t r, g, b;
    uint8_t c, w;
} light_color_t;

// What one LED on an addressable strip is given. No cold/warm split: the wire protocol carries
// at most a single white element (SK6812), and a plain WS2812 not even that. Eight bits per
// channel is the protocol's own width, so nothing here is configurable.
typedef struct {
    uint8_t r, g, b, w;
} light_pixel_t;

// What one PWM channel is given. Wider than a byte because the LEDC timer's duty resolution is
// negotiated against the configured frequency and reaches 11 bits at 20 kHz - the extra range is
// the point, not a rounding artifact.
typedef struct {
    uint16_t r, g, b;
    uint16_t c, w;
} light_duty_t;

// The widest duty light_duty_t can carry. A slow enough timer will offer more (19 bits at
// 100 Hz), and configuring it that wide while feeding it these values would cap the light at a
// fraction of full output - so the timer is held to this too. Nothing is lost: the eye runs out
// of steps long before sixteen bits do.
#define LIGHT_DUTY_MAX_RESOLUTION 16

// Builds the lookup tables. Call once, before anything else here.
void light_color_init(void);

// Standard HSV -> RGB. hue is degrees (0-359), saturation and value are percentages (0-100).
// The render path always passes value = 100 and leaves dimming to the output stages below;
// value is there for callers reporting a color for a UI to draw rather than a drive level.
void light_color_hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value, uint8_t *red,
                            uint8_t *green, uint8_t *blue);

// The two output stages, and the only places gamma or brightness is applied. Each one carries
// the brightness curve its hardware can afford, so neither takes a flag saying which to use.
light_pixel_t light_color_to_pixel(light_color_t color, uint8_t brightness);
light_duty_t light_color_to_duty(light_color_t color, uint8_t brightness, uint8_t resolution);
