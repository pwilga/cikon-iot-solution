#pragma once

// Public API of the light driver: enumerate lights, ask what they can do, read their state and
// change it.
//
// light_config_t and lights[] are deliberately absent. That struct embeds
// fx_state.buf[LIGHT_EFFECTS_MAX_LEDS], and both its size and the presence of several members
// come from compile definitions the light component keeps PRIVATE. A caller compiled without
// them would agree on the struct's name and disagree on its layout - a silent ABI mismatch
// rather than a compile error. Every type below is fixed-layout on purpose: no conditional
// members, no arrays sized by a definition.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "light_color.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- lifecycle ---------------------------------------------------------------------------

// Parses the configuration, restores persisted state, brings up LEDC/GPIO/led_strip, drives
// every light to its initial state and starts the effects task.
esp_err_t light_init(void);
esp_err_t light_shutdown(void);

// Writes state to NVS if it changed since the last write. Call periodically; how often is the
// caller's policy. Does nothing when persistence is disabled.
void light_save_state(void);

// --- identity ----------------------------------------------------------------------------

// Lights this firmware was built for, fixed at compile time by the device's lights.toml.
size_t light_count(void);

// Sanitized name, stable for the process lifetime, NULL past the end. This is the string a
// light is addressed by from the outside.
const char *light_name(size_t index);

// Index of the light with this sanitized name, or -1.
int8_t light_index_by_name(const char *name);

// --- capabilities ------------------------------------------------------------------------

// What a light is able to do, fixed at configuration time.
typedef struct {
    bool is_switch;      // pure on/off relay: no brightness, no color
    bool has_color;      // R+G+B present, so hue and saturation mean something
    bool has_cct;        // C and W both present, so they can be mixed
    bool has_cold_white; // a physical cold-white channel exists
    bool has_warm_white; // a physical warm-white channel exists
    bool has_effects;    // an addressable strip, with the effects engine in this build
} light_caps_t;

bool light_get_caps(size_t index, light_caps_t *caps);

// --- reading state -----------------------------------------------------------------------

// A light's state in full.
typedef struct {
    bool on;
    uint8_t brightness; // 0-100; meaningless when caps.is_switch
    uint16_t cct;       // 0-100 cool share; meaningful when caps.has_cct
    const char *effect; // static storage, NULL when caps.has_effects is false

    // At full output - brightness sits in its own field above and is deliberately not folded
    // in, because scaling a saturated color to 8 bits at the bottom of the slider lands on the
    // wrong hue.
    //
    //   .r/.g/.b - hue and saturation at full value in color mode; an approximate warm-to-cool
    //              tint derived from cct in white mode. Independent of `on`, so a picker keeps
    //              showing the chosen color while the light is off.
    //   .c/.w    - the cold/warm split at full output. Zero in color mode, and zero when off.
    //
    // The two halves treat `on` differently on purpose, and consumers are built against that.
    light_color_t color;
} light_state_t;

bool light_get_state(size_t index, light_state_t *state);

// --- changing state ----------------------------------------------------------------------

// One bit each, so a change can name several fields at once by OR-ing them together.
typedef enum {
    LIGHT_FIELD_ON = 1u << 0,
    LIGHT_FIELD_BRIGHTNESS = 1u << 1,
    LIGHT_FIELD_HUE = 1u << 2,
    LIGHT_FIELD_SATURATION = 1u << 3,
    LIGHT_FIELD_CCT = 1u << 4,
    LIGHT_FIELD_EFFECT = 1u << 5,
    LIGHT_FIELD_EFFECT_SPEED = 1u << 6,
    LIGHT_FIELD_EFFECT_INTENSITY = 1u << 7,
    LIGHT_FIELD_HUE2 = 1u << 8,
    LIGHT_FIELD_SATURATION2 = 1u << 9,
    LIGHT_FIELD_BRIGHTNESS2 = 1u << 10,
} light_field_t;

// A partial change: only the fields named in `fields` are read, the rest may be left
// uninitialized. The mask is what separates "hue was not sent" from "hue was sent as 0" -
// both the mode switching below and the implicit turn-on depend on that distinction.
// Values are clamped by the driver; callers must not pre-clamp.
typedef struct {
    uint32_t fields; // OR of light_field_t
    bool on;
    uint8_t brightness;
    uint16_t hue;
    uint8_t saturation;
    uint16_t cct;
    const char *effect; // borrowed for the call only; an unknown name is logged and skipped
    uint8_t effect_speed;
    uint8_t effect_intensity;
    uint16_t hue2;
    uint8_t saturation2;
    uint8_t brightness2;
} light_state_change_t;

// Applies every named field and then drives the hardware once, however many were set - one
// command carrying hue, saturation, brightness and on must not produce four separate fades.
//
// Precedence between the color and white modes:
//   - cct wins over hue/saturation on a light that has any white channel, and selects white
//     mode; on a light without one it is ignored
//   - otherwise hue or saturation selects color mode
//   - brightness applies in either mode
//   - if `on` is absent but anything else was set, the light turns on
//   - changing the effect resets that effect's scratch state
esp_err_t light_apply_change(size_t index, const light_state_change_t *change);

esp_err_t light_set_on(size_t index, bool on);

// Separate from light_set_on() because flipping `on` is a read-modify-write that must not be
// split across two calls by the caller.
esp_err_t light_toggle(size_t index);

// --- effect catalogue --------------------------------------------------------------------

// Answerable in every build: the count is 0 and names are NULL where the effects engine was
// left out.
size_t light_effect_count(void);
const char *light_effect_name(size_t effect);

#ifdef __cplusplus
}
#endif
