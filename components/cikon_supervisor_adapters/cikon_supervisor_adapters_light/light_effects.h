#pragma once

// Private, component-internal header - only light.c includes this (guarded by
// LIGHT_EFFECTS_BUILD, see CMakeLists.txt). Not installed under include/.

#include <stdbool.h>

typedef enum {
    LIGHT_EFFECT_NONE = 0, // "solid" - today's static fill, no animation
    LIGHT_EFFECT_BLINK,
    LIGHT_EFFECT_BREATHE,
    LIGHT_EFFECT_RAINBOW,
} light_effect_t;

// Case-insensitive name -> enum lookup for the cmnd "effect" field (e.g. "rainbow").
// Returns false (and leaves *out untouched) if name doesn't match any known effect.
bool light_effect_from_name(const char *name, light_effect_t *out);

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
