#pragma once

// Private, component-internal header: the light's own state (light_config_t, lights[]) plus the
// handful of functions the component's .c files share. Not installed under include/ -
// light_adapter.h remains the only public surface.
//
// Holding the state here, rather than behind a getter/setter layer, lets light.c,
// light_config.c and light_effects.c all treat lights[] as one shared thing - they are one
// logical adapter, split by concern rather than by ownership. Pure color math lives in
// light_color.h instead, which stays free of ESP-IDF headers so it can be compiled and checked
// on a host.

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h" // IWYU pragma: keep - gpio_num_t used in light_channel_t
#include "driver/ledc.h" // IWYU pragma: keep - ledc_channel_t used in light_channel_t
#include "light_color.h"

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

// Fills lights[] from CONFIG_LIGHT_GPIO_LIST. Defined in light_config.c, called once at init.
void light_config_parse(void);

// The effect catalogue. Defined in light.c and answerable in every build - light_effects.c,
// which holds the rows, is compiled only when the effects engine is enabled. Count is 0 and
// name is NULL in builds without it.
size_t light_effect_count(void);
const char *light_effect_name(size_t effect);
int8_t light_effect_index(const char *name);

// The light's on/color_mode/white/cct state as one raw color, at full output and before gamma
// - what light_color.h calls the input to the output stage. Defined in light.c, shared so
// light_effects.c renders the solid (effect == NONE) case from the same source of truth.
light_rgbcw_t light_compute_color(light_config_t *light);

#ifdef LIGHT_HAS_ADDRESSABLE
// Sets one pixel from a raw (full-output, pre-gamma) color, dispatching to the has_white-aware
// led_strip call. Defined in light.c, shared so per-pixel effects in light_effects.c neither
// duplicate the has_white branch nor have to think about gamma or the brightness slider -
// light_color_to_levels() runs inside. color.w is ignored when has_white is false.
void light_addressable_set_pixel(led_strip_handle_t handle, uint16_t i, bool has_white,
                                 light_rgbcw_t color, uint8_t brightness);

// led_strip has no "fill all pixels" of its own - only per-pixel set_pixel/set_pixel_rgbw.
// Defined in light.c, shared with light_effects.c so both use the same fill loop. color.w is
// ignored when has_white is false.
void light_addressable_fill(led_strip_handle_t handle, uint16_t count, bool has_white,
                            light_rgbcw_t color, uint8_t brightness);
#endif
