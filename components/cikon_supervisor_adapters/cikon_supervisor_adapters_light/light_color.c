// Color pipeline
// ==============
// Two things happen between a picked color and what the LEDs get: gamma correction (an LED's
// output is linear in drive level, an sRGB color is not) and dimming. Their order is the whole
// ballgame, and having it backwards is what made a color walk up the brightness slider.
//
// Dimming first and gamma last - gamma(color * brightness) - reads as the obvious arrangement
// and is wrong. Gamma squashes small inputs hard: at 2.2, an input of 11 lands on 0. So once
// brightness has already shrunk every channel, a saturated color's weak channels round away
// while its strong one survives. FF6E54 at 10% dims to (25,11,8), which gamma turns into
// (2,0,0) - pure red. At 20% it lands on (7,1,1), an orange-yellow. The hue climbs with the
// slider instead of staying put.
//
// So gamma goes first, at the color's full 8-bit precision, and brightness scales the result
// linearly afterwards - one factor across every channel, so the ratios between them (which is
// what hue is) hold all the way down. FF6E54 gamma-corrects once to (255,40,22), and 10% of
// that is (26,4,2): the same color, darker. This is WLED's ordering, which says as much in its
// own source - wled00/FX_fcn.cpp, in WS2812FX::show(): "applying gamma after brightness has too
// much color loss". It gamma-corrects each pixel there, and the bus applies brightness
// afterwards in color_fade().
//
// The trade is that the slider now moves linearly in emitted light rather than in perceived
// brightness, so its lower half feels less finely graded. That is also what WLED ships
// (gammaCorrectBri off by default), and it is the only half of the trade 8-bit channels can
// hold: gamma on the brightness instead would put a 10% slider at 0.6% of full scale, below
// what a WS2812 can represent at all.
//
// Eight bits is also where this stops. At the very bottom of the slider a saturated color has
// nowhere left to put its weak channels - FF6E54 at 1% comes out pure red - and no arrangement
// of these two operations fixes that, only more bits or dithering would. WLED has the same
// artifact on WS2812, for the same reason.

#include <math.h>

#include "light_color.h"

#define LIGHT_GAMMA 2.2f

static uint8_t gamma_lut[256];

void light_color_init(void) {
    gamma_lut[0] = 0;
    for (int i = 1; i < 256; i++) {
        gamma_lut[i] = (uint8_t)(powf((float)i / 255.0f, LIGHT_GAMMA) * 255.0f + 0.5f);
    }
}

// Scales one gamma-corrected channel to `brightness` percent, rounding rather than truncating,
// and holds a channel that still carries a real share of the color at 1 instead of letting it
// drop out - WLED's "video" scaling (color_fade() with video = true). floor_threshold is what
// "a real share" means: the RGB triple passes the dominant channel's quarter, since a channel
// lost there shifts the hue, while a white channel passes 0, having no hue to shift and only
// needing to stay lit.
static uint8_t light_color_scale_channel(uint8_t channel, uint8_t brightness,
                                         uint8_t floor_threshold) {
    uint8_t scaled = (uint8_t)(((uint16_t)channel * brightness + 50) / 100);
    return (scaled == 0 && channel > floor_threshold) ? 1 : scaled;
}

light_rgbcw_t light_color_to_levels(light_rgbcw_t color, uint8_t brightness) {
    light_rgbcw_t out = {0};

    if (brightness == 0) {
        return out;
    }
    if (brightness > 100) {
        brightness = 100;
    }

    uint8_t r = gamma_lut[color.r], g = gamma_lut[color.g], b = gamma_lut[color.b];
    // Anything below a quarter of the strongest channel is a tint too faint to defend: pinned
    // at 1 while the rest of the color dims past it, it would end up over-represented and drag
    // the color toward white instead of letting it simply get darker.
    uint8_t dominant = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
    uint8_t threshold = (uint8_t)((dominant >> 2) + 1);

    out.r = light_color_scale_channel(r, brightness, threshold);
    out.g = light_color_scale_channel(g, brightness, threshold);
    out.b = light_color_scale_channel(b, brightness, threshold);
    out.c = light_color_scale_channel(gamma_lut[color.c], brightness, 0);
    out.w = light_color_scale_channel(gamma_lut[color.w], brightness, 0);
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
