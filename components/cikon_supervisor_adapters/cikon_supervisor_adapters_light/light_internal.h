#pragma once

// Private, component-internal header shared between light.c and light_effects.c. Not
// installed under include/ - light_adapter.h remains the only public surface of this
// component. Splitting the struct definitions out here (rather than making light_effects.c
// a copy) lets both files treat lights[] as one shared piece of state without a
// getter/setter layer - they're still one logical adapter, just split across two files by
// concern (config/cmnd/NVS vs. animation timing).

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h" // IWYU pragma: keep - gpio_num_t used in light_channel_t
#include "driver/ledc.h" // IWYU pragma: keep - ledc_channel_t used in light_channel_t

#ifdef LIGHT_HAS_ADDRESSABLE
#include "led_strip.h" // IWYU pragma: keep - led_strip_handle_t/led_color_component_format_t used below
#endif

#define LIGHT_MAX_CHANNELS 5

typedef enum {
    CH_NONE = 0,
    CH_RED,
    CH_GREEN,
    CH_BLUE,
    CH_COLD_WHITE,
    CH_WARM_WHITE,
    CH_SWITCH,
    CH_ADDRESSABLE
} light_channel_role_t;

typedef struct {
    gpio_num_t gpio;
    light_channel_role_t role;
    ledc_channel_t ledc_ch; // unused when role == CH_SWITCH or CH_ADDRESSABLE
    bool active_level;      // physical level meaning "on"; only meaningful when role == CH_SWITCH
#ifdef LIGHT_HAS_ADDRESSABLE
    uint16_t led_count;                            // only meaningful when role == CH_ADDRESSABLE
    led_color_component_format_t led_color_format; // only meaningful when role == CH_ADDRESSABLE
    bool has_white_channel; // format has a 4th (W) component; only for CH_ADDRESSABLE
#endif
} light_channel_t;

#if LIGHT_EFFECTS_BUILD
#ifndef LIGHT_EFFECTS_MAX_LEDS
#define LIGHT_EFFECTS_MAX_LEDS 300 // fallback if CMake didn't inject it
#endif

// Generic per-effect scratch state, WLED SEGENV equivalent. Static, not malloc'd - since
// LIGHT_EFFECTS_BUILD is a compile-time flag, this either lives for the light's whole lifetime
// or isn't in the binary at all, so there's nothing to lazily allocate or free.
// LIGHT_EFFECTS_MAX_LEDS is computed by CMakeLists.txt from CONFIG_LIGHT_GPIO_LIST's actual
// addressable channel(s), not guessed.
typedef struct {
    uint16_t aux0;
    uint16_t aux1;
    uint32_t step;
    uint32_t data0;
    uint8_t buf[LIGHT_EFFECTS_MAX_LEDS]; // per-pixel scratch (e.g. Fire2012's "heat" array)
} light_effect_state_t;
#endif

typedef struct {
    light_channel_t channels[LIGHT_MAX_CHANNELS];
    uint8_t channel_count; // 0 == unused slot (sentinel)
    bool has_color;        // has R+G+B
    bool has_white;        // has C and/or W
    bool has_cct;          // has C and W together (real cold/warm mixing)
    bool is_switch;        // true = pure on/off relay light (role S) - no PWM/brightness at all
#ifdef LIGHT_HAS_ADDRESSABLE
    bool is_addressable; // true = WS2812/SK6812-style strip (role N) - driven via led_strip, not
                         // LEDC
    led_strip_handle_t addressable_handle;
#endif
    char name[16];
    bool on;
    bool color_mode;  // true = RGB output active, false = C/W output active
    uint16_t hue;     // 0-360, color mode
    uint8_t sat, val; // 0-100, color mode (val also doubles as white-mode brightness)
    uint16_t cct;     // 0-100, white mode cold/warm ratio
#if LIGHT_EFFECTS_BUILD
    // Stored as a plain uint8_t (not light_effect_t) so this header doesn't need to depend
    // on light_effects.h - light.c/light_effects.c cast to/from light_effect_t as needed.
    uint8_t effect;           // light_effect_t; LIGHT_EFFECT_NONE (0) = solid, today's behavior
    uint8_t effect_speed;     // 0-100, default 50
    uint8_t effect_intensity; // 0-100, default 50
    // Generic second color (WLED SEGCOLOR(1) equivalent) - role defined by whichever effect
    // uses it (e.g. android's background), not by this field's name. val2 defaults to 0 (true
    // black) independently of val - sat2=0 alone would still be lit (grey) at the main val.
    uint16_t hue2;
    uint8_t sat2, val2;
    // Generic per-effect scratch state, WLED SEGENV equivalent - each effect interprets these
    // fields however it needs (position, size, phase accumulator, per-pixel heat/trail...).
    // Reset to zero whenever the effect changes (light.c:light_apply()), so stale state from
    // one effect never leaks into another.
    light_effect_state_t fx_state;
#endif
} light_config_t;

extern light_config_t lights[CONFIG_LIGHT_MAX_COUNT + 1]; // +1 sentinel

// Shared color math, defined in light.c (kept there since gamma_lut, used by the
// white/CCT branch, is private to that file).
void light_hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value, uint8_t *red, uint8_t *green,
                      uint8_t *blue);
void light_compute_rgbcw(light_config_t *light, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *c,
                         uint8_t *w);

#ifdef LIGHT_HAS_ADDRESSABLE
// Sets one pixel, dispatching to the has_white-aware led_strip call. Defined in light.c,
// shared so per-pixel effects in light_effects.c don't duplicate the has_white branch. w is
// ignored when has_white is false.
void light_addressable_set_pixel(led_strip_handle_t handle, uint16_t i, bool has_white, uint8_t r,
                                 uint8_t g, uint8_t b, uint8_t w);

// led_strip has no "fill all pixels" of its own - only per-pixel set_pixel/set_pixel_rgbw.
// Defined in light.c, shared with light_effects.c so both use the same fill loop. w is
// ignored when has_white is false.
void light_addressable_fill(led_strip_handle_t handle, uint16_t count, bool has_white, uint8_t r,
                            uint8_t g, uint8_t b, uint8_t w);
#endif
