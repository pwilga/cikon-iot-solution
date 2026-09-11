// Gamma correction runs first, at full precision, and brightness scales the result linearly
// afterwards. The other order - gamma(color * brightness) - is what made FF6E54 walk from red
// through yellow as the slider rose: gamma rounds small inputs to zero, so once dimming has
// shrunk every channel, a saturated color's weak ones drop out while its strong one survives.
// This is WLED's ordering (wled00/FX_fcn.cpp, WS2812FX::show()). What that costs is a slider
// linear in emitted light rather than in what the eye reads, which the PWM path buys back with
// a curve on the brightness (see cie_lightness) and the strip path cannot, having only eight
// bits to spend.

#include <math.h>

#include "light_color.h"

#define LIGHT_GAMMA 2.2f

// sRGB channel (0-255) -> linear light, at 16 bits. Wider than either output needs, on purpose:
// gamma runs once here at more precision than any hardware has, so narrowing to the output's own
// width later is the only rounding a color suffers. An 8-bit table would cap a PWM channel at
// 256 distinct levels no matter how many bits the timer offers.
static uint16_t gamma_lut[256];

void light_color_init(void) {
    gamma_lut[0] = 0;
    for (int i = 1; i < 256; i++) {
        gamma_lut[i] = (uint16_t)(powf((float)i / 255.0f, LIGHT_GAMMA) * 65535.0f + 0.5f);
    }
}

// Below what value a channel stops carrying a real share of the color. A channel under a quarter
// of the strongest one is a tint too faint to defend: pinned at 1 while the rest of the color
// dims past it, it would end up over-represented and drag the color toward white instead of
// letting it simply get darker. White channels have no ratio to hold, so they pass 0 and only
// need to stay lit.
static uint16_t rgb_floor(uint16_t r, uint16_t g, uint16_t b) {
    uint16_t dominant = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
    return (uint16_t)((dominant >> 2) + 1);
}

// The strip's own width. Gamma is stored at 16 bits for the PWM path's sake; a pixel takes it
// back at eight, which is all the wire protocol carries, and dims there - the same width WLED
// dims at.
static uint8_t gamma_pixel(uint8_t srgb) {
    return (uint8_t)(((uint32_t)gamma_lut[srgb] * 255u + 32767u) / 65535u);
}

// Both scalers round rather than truncate, and hold a channel above the floor at 1 rather than
// letting it drop out - WLED's "video" scaling (color_fade() with video = true).
static uint8_t scale_pixel_channel(uint8_t channel, uint8_t brightness, uint8_t floor_threshold) {
    uint8_t scaled = (uint8_t)(((uint16_t)channel * brightness + 50) / 100);
    return (scaled == 0 && channel > floor_threshold) ? 1 : scaled;
}

// Where a slider setting lands in emitted light, as a fraction of full output. The eye reads
// light roughly as its cube root, so a slider that divides light evenly spends nearly all of its
// visible travel in the bottom third; this is the CIE 1931 lightness curve, which divides
// *perceived* brightness evenly instead. Straight below 21/255 rather than cubic, because down
// there the cube would ask for less duty than a timer step and the light would simply go out.
// Constants are WLED's (bus_manager.cpp, BusPwm::show()), kept in its 0-255 terms so the two
// stay comparable.
static float cie_lightness(uint8_t brightness) {
    float level = brightness * 255.0f / 100.0f;

    if (level < 21.0f) {
        return level / 2300.0f;
    }
    float t = (level + 41.0f) / 296.0f;
    return t * t * t;
}

// One curve factor across every channel, so the ratios between them - the hue - survive the
// dimming untouched; only the total output moves. Narrows once, at the end, to whatever width
// the timer offers.
static uint16_t scale_duty_channel(uint16_t channel, float lightness, uint32_t full_scale,
                                   uint16_t floor_threshold) {
    uint16_t scaled = (uint16_t)((float)channel / 65535.0f * lightness * (float)full_scale + 0.5f);
    return (scaled == 0 && channel > floor_threshold) ? 1 : scaled;
}

light_pixel_t light_color_to_pixel(light_color_t color, uint8_t brightness) {
    light_pixel_t out = {0};

    if (brightness == 0) {
        return out;
    }
    if (brightness > 100) {
        brightness = 100;
    }

    uint8_t r = gamma_pixel(color.r), g = gamma_pixel(color.g), b = gamma_pixel(color.b);
    uint8_t threshold = (uint8_t)rgb_floor(r, g, b);

    out.r = scale_pixel_channel(r, brightness, threshold);
    out.g = scale_pixel_channel(g, brightness, threshold);
    out.b = scale_pixel_channel(b, brightness, threshold);
    out.w = scale_pixel_channel(gamma_pixel(color.w), brightness, 0);
    return out;
}

light_duty_t light_color_to_duty(light_color_t color, uint8_t brightness, uint8_t resolution) {
    light_duty_t out = {0};

    if (brightness == 0 || resolution == 0) {
        return out;
    }
    if (brightness > 100) {
        brightness = 100;
    }
    if (resolution > LIGHT_DUTY_MAX_RESOLUTION) {
        resolution = LIGHT_DUTY_MAX_RESOLUTION;
    }

    uint32_t full_scale = (1u << resolution) - 1u;
    float lightness = cie_lightness(brightness);
    uint16_t r = gamma_lut[color.r], g = gamma_lut[color.g], b = gamma_lut[color.b];
    uint16_t threshold = rgb_floor(r, g, b);

    out.r = scale_duty_channel(r, lightness, full_scale, threshold);
    out.g = scale_duty_channel(g, lightness, full_scale, threshold);
    out.b = scale_duty_channel(b, lightness, full_scale, threshold);
    out.c = scale_duty_channel(gamma_lut[color.c], lightness, full_scale, 0);
    out.w = scale_duty_channel(gamma_lut[color.w], lightness, full_scale, 0);
    return out;
}

void light_color_hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value, uint8_t *red,
                            uint8_t *green, uint8_t *blue) {

    hue %= 360;
    float saturation_frac = (saturation > 100 ? 100 : saturation) / 100.0f;
    float value_frac = (value > 100 ? 100 : value) / 100.0f;
    float chroma = value_frac * saturation_frac;
    float sector = hue / 60.0f;
    float second_component = chroma * (1.0f - fabsf(fmodf(sector, 2.0f) - 1.0f));
    float match = value_frac - chroma;
    float r_prime, g_prime, b_prime;

    switch ((int)sector) {
    case 0:
        r_prime = chroma, g_prime = second_component, b_prime = 0.0f;
        break;
    case 1:
        r_prime = second_component, g_prime = chroma, b_prime = 0.0f;
        break;
    case 2:
        r_prime = 0.0f, g_prime = chroma, b_prime = second_component;
        break;
    case 3:
        r_prime = 0.0f, g_prime = second_component, b_prime = chroma;
        break;
    case 4:
        r_prime = second_component, g_prime = 0.0f, b_prime = chroma;
        break;
    default:
        r_prime = chroma, g_prime = 0.0f, b_prime = second_component;
        break;
    }

    *red = (uint8_t)((r_prime + match) * 255.0f + 0.5f);
    *green = (uint8_t)((g_prime + match) * 255.0f + 0.5f);
    *blue = (uint8_t)((b_prime + match) * 255.0f + 0.5f);
}
