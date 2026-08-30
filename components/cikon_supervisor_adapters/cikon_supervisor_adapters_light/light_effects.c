#include <math.h>
#include <stddef.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep - required before freertos/task.h
#include "freertos/task.h"

#include "led_strip.h"

#include "light_effects.h"
#include "light_internal.h"

#define TAG "cikon:adapter:light:effects"
#define LIGHT_EFFECTS_STACK_SIZE 4096

static TaskHandle_t s_task_handle = NULL;

static const struct {
    const char *name;
    light_effect_t effect;
} k_effect_names[] = {
    {"solid", LIGHT_EFFECT_NONE},
    {"blink", LIGHT_EFFECT_BLINK},
    {"breathe", LIGHT_EFFECT_BREATHE},
    {"rainbow", LIGHT_EFFECT_RAINBOW},
};

bool light_effect_from_name(const char *name, light_effect_t *out) {
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_effect_names) / sizeof(k_effect_names[0]); i++) {
        if (strcasecmp(name, k_effect_names[i].name) == 0) {
            *out = k_effect_names[i].effect;
            return true;
        }
    }
    return false;
}

static void light_effects_render(light_config_t *light, uint32_t now_ms) {
    if (!light->addressable_handle) {
        return;
    }

    light_channel_t *ch = &light->channels[0]; // addressable lights have exactly one channel
    uint16_t led_count = ch->led_count;
    bool has_white = ch->has_white_channel;

    if (!light->on) {
        light_addressable_fill(light->addressable_handle, led_count, has_white, 0, 0, 0, 0);
        led_strip_refresh(light->addressable_handle);
        return;
    }

    light_effect_t effect = (light_effect_t)light->effect;

    // Timing modeled on WLED's mode_blink/mode_breath (FX.cpp): derive state directly from
    // an absolute timestamp and speed, no per-frame phase accumulator - avoids drift and
    // needs no persistent state in light_config_t. Exact constants below are a starting
    // point for hardware bring-up, not derived from a spec.
    switch (effect) {
    case LIGHT_EFFECT_BLINK: {
        // Animates brightness as a square wave: fully on for half the cycle, off for the
        // other half. Hue/saturation stay at the light's configured color throughout.
        uint32_t cycle_ms = 200 + (uint32_t)(100 - light->effect_speed) * 18;
        uint8_t level = ((now_ms % cycle_ms) < cycle_ms / 2) ? 100 : 0;
        uint8_t r, g, b;
        light_hsv_to_rgb(light->hue, light->sat, (uint8_t)((light->val * level) / 100), &r, &g, &b);
        light_addressable_fill(light->addressable_handle, led_count, has_white, r, g, b, 0);
        break;
    }
    case LIGHT_EFFECT_BREATHE: {
        // Ported from WLED's mode_breath (FX.cpp), not reinvented: it blends the target
        // color toward black using a 0-255 factor with a floor (never fully off) and a
        // pulse that only occupies the first quarter of the cycle, resting at the floor
        // the rest of the time - not a plain symmetric sine over the full 0-100 range.
        // The earlier gamma-on-a-0-100-scale attempt still looked stepped because the
        // real issue was re-quantizing brightness through our 0-100 HSV "value"
        // parameter (light_hsv_to_rgb rounds that to 0-255 internally) - computing the
        // base color once and blending in RGB space at full 0-255 precision, like WLED
        // does, is what actually fixes it.
        float period_ms = 900.0f + (float)(100 - light->effect_speed) * 60.0f;
        float t = fmodf((float)now_ms, period_ms) / period_ms; // 0..1 over one cycle
        float pulse_phase = t * 4.0f;                          // pulse occupies t in [0, 0.25)
        float shape = 0.0f;
        if (pulse_phase < 1.0f) {
            float triangle = (pulse_phase < 0.5f) ? (pulse_phase * 2.0f) : (2.0f - pulse_phase * 2.0f);
            shape = sinf(triangle * (float)M_PI / 2.0f); // eased rise/fall, 0..1
        }
        uint8_t lum = 30 + (uint8_t)(shape * 225.0f); // 30..255, never fully black
        uint8_t br, bg, bb;
        light_hsv_to_rgb(light->hue, light->sat, light->val, &br, &bg, &bb);
        uint8_t r = (uint8_t)(((uint16_t)br * lum) / 255);
        uint8_t g = (uint8_t)(((uint16_t)bg * lum) / 255);
        uint8_t b = (uint8_t)(((uint16_t)bb * lum) / 255);
        light_addressable_fill(light->addressable_handle, led_count, has_white, r, g, b, 0);
        break;
    }
    case LIGHT_EFFECT_RAINBOW: {
        // Animates hue instead of brightness: brightness stays at the light's configured
        // val throughout, and hue rotates over time (base_hue) with an additional per-pixel
        // offset so the color sweeps visibly along the strip instead of all LEDs matching.
        uint32_t base_hue = (now_ms * ((light->effect_speed >> 2) + 2) / 8) % 360;
        for (uint16_t i = 0; i < led_count; i++) {
            uint16_t hue = (uint16_t)((base_hue + (uint32_t)i * 360 / led_count) % 360);
            uint8_t r, g, b;
            light_hsv_to_rgb(hue, 100, light->val, &r, &g, &b);
            if (has_white) {
                led_strip_set_pixel_rgbw(light->addressable_handle, i, r, g, b, 0);
            } else {
                led_strip_set_pixel(light->addressable_handle, i, r, g, b);
            }
        }
        break;
    }
    case LIGHT_EFFECT_NONE:
    default: {
        uint8_t r, g, b, c, w;
        light_compute_rgbcw(light, &r, &g, &b, &c, &w);
        light_addressable_fill(light->addressable_handle, led_count, has_white, r, g, b, w);
        break;
    }
    }

    led_strip_refresh(light->addressable_handle);
}

// Only lights that are on AND actively animating justify the tight frame-period wake-up;
// everything else (off, or on but solid) only needs a render on the next notify().
static bool light_effects_has_active_animation(void) {
    for (int i = 0; lights[i].channel_count != 0; i++) {
        if (lights[i].is_addressable && lights[i].on && lights[i].effect != LIGHT_EFFECT_NONE) {
            return true;
        }
    }
    return false;
}

static void light_effects_task(void *arg) {
    (void)arg;
    uint32_t now_ms;

    // Initial render covers the boot state (and any notify() calls made by light_apply()/
    // light_adapter_init() before this task existed, which were silently dropped) - without
    // this, a freshly configured strip would stay dark until the first cmnd arrives.
    now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; lights[i].channel_count != 0; i++) {
        if (lights[i].is_addressable) {
            light_effects_render(&lights[i], now_ms);
        }
    }

    for (;;) {
        TickType_t timeout = light_effects_has_active_animation()
                                  ? pdMS_TO_TICKS(1000 / CONFIG_LIGHT_EFFECTS_FPS)
                                  : portMAX_DELAY;
        ulTaskNotifyTake(pdTRUE, timeout);

        now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        for (int i = 0; lights[i].channel_count != 0; i++) {
            if (lights[i].is_addressable) {
                light_effects_render(&lights[i], now_ms);
            }
        }
    }
}

void light_effects_task_start(void) {
    if (s_task_handle) {
        return;
    }

    bool any_addressable = false;
    for (int i = 0; lights[i].channel_count != 0; i++) {
        if (lights[i].is_addressable) {
            any_addressable = true;
            break;
        }
    }
    if (!any_addressable) {
        return;
    }

    xTaskCreate(light_effects_task, "light_fx", LIGHT_EFFECTS_STACK_SIZE, NULL,
                CONFIG_SUPERVISOR_TASK_PRIORITY > 0 ? CONFIG_SUPERVISOR_TASK_PRIORITY - 1 : 0,
                &s_task_handle);
}

void light_effects_task_stop(void) {
    if (!s_task_handle) {
        return;
    }
    TaskHandle_t handle = s_task_handle;
    s_task_handle = NULL;
    vTaskDelete(handle);
}

void light_effects_notify(void) {
    if (s_task_handle) {
        xTaskNotifyGive(s_task_handle);
    }
}
