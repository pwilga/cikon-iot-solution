#pragma once

// Private, component-internal header - only light.c includes this (guarded by
// LIGHT_EFFECTS_BUILD, see CMakeLists.txt). Not installed under include/.

#include <stddef.h>
#include <stdint.h>

#include "light_internal.h" // light_config_t, in the render function signature below

typedef enum {
    LIGHT_EFFECT_NONE = 0, // "solid" - today's static fill, no animation
    LIGHT_EFFECT_BLINK,
    LIGHT_EFFECT_BREATHE,
    LIGHT_EFFECT_RAINBOW,
    LIGHT_EFFECT_WASHING_MACHINE,
    LIGHT_EFFECT_ANDROID,
    LIGHT_EFFECT_FIRE_2012,
    LIGHT_EFFECT_TWINKLE,
    LIGHT_EFFECT_METEOR,
} light_effect_t;

// The effect registry: one row per effect, indexed by light_effect_t. Exposed as data rather
// than behind accessors because light.c has to answer "which effects exist" in every build,
// including the ones where this file is not compiled at all - so the questions are answered
// there, guarded by LIGHT_EFFECTS_BUILD, and this file only supplies the rows.
typedef struct {
    const char *name; // as it travels over MQTT, and as the cmnd "effect" field accepts it
    void (*fn)(light_config_t *light, uint32_t now_ms);
} light_effect_entry_t;

extern const light_effect_entry_t light_effect_table[];
extern const size_t light_effect_table_size;

// Creates the effects task if >=1 configured light is addressable; no-op otherwise. Must be
// called once, after light_adapter_init() has finished setting up all addressable_handle
// values (the task's own startup render needs them).
void light_effects_task_start(void);

// Deletes the effects task if running. Must be called before any addressable_handle is
// deleted, so the task never touches a freed handle.
void light_effects_task_stop(void);

// Wakes the effects task to re-render on the next available tick, after light.c changes any
// addressable light's state (color/on/effect/speed). Safe to call even if the task hasn't
// been started yet (no-op) - light_effects_task_start()'s own initial render covers that case.
void light_effects_notify(void);
