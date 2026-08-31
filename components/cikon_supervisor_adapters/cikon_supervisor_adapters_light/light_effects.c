#include <math.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep - required before freertos/task.h
#include "freertos/task.h"

#include "led_strip.h"

#include "light_effects.h"
#include "light_internal.h"

#define TAG "cikon:adapter:light:effects"
#define LIGHT_EFFECTS_STACK_SIZE 4096

static TaskHandle_t s_task_handle = NULL;

static void light_effects_render(light_config_t *light, uint32_t now_ms);

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

// ============================================================================================
// Effects - add a new one here: write an effect_*() function, add one row to k_effects[]
// below (and a value in light_effect_t, light_effects.h). Nothing above this line needs to
// change.
// ============================================================================================

typedef void (*light_effect_fn_t)(light_config_t *light, uint32_t now_ms);

// "solid" - static fill, no animation. Also the fallback for an unrecognized effect id.
static void effect_solid(light_config_t *light, uint32_t now_ms) {
    (void)now_ms;

    uint8_t r, g, b, c, w;
    light_compute_rgbcw(light, &r, &g, &b, &c, &w);
    light_addressable_fill(light->addressable_handle, light->channels[0].led_count,
                           light->channels[0].has_white_channel, r, g, b, w);
}

// Square-wave brightness (WLED mode_blink), driven off an absolute timestamp so there's no
// per-frame phase state to keep in light_config_t.
static void effect_blink(light_config_t *light, uint32_t now_ms) {

    uint32_t cycle_ms = 200 + (uint32_t)(100 - light->effect_speed) * 18;
    uint8_t level = ((now_ms % cycle_ms) < cycle_ms / 2) ? 100 : 0;
    uint8_t r, g, b;

    light_hsv_to_rgb(light->hue, light->sat, (uint8_t)((light->val * level) / 100), &r, &g, &b);
    light_addressable_fill(light->addressable_handle, light->channels[0].led_count,
                           light->channels[0].has_white_channel, r, g, b, 0);
}

// Ported from WLED's mode_breath: eased pulse in the first quarter of the cycle, resting at a
// floor (never fully black) the rest of the time - not a plain sine over 0-100.
static void effect_breathe(light_config_t *light, uint32_t now_ms) {

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
    light_addressable_fill(light->addressable_handle, light->channels[0].led_count,
                           light->channels[0].has_white_channel, r, g, b, 0);
}

// Ported from WLED's mode_rainbow: the whole strip cycles through one hue over time (not a
// per-pixel gradient sweep). effect_speed (0-100 here) is rescaled to WLED's native 0-255
// speed range so the same "(speed>>2)+2" timing constant feels the same across the full
// slider, not capped at ~42% of WLED's max rate.
static void effect_rainbow(light_config_t *light, uint32_t now_ms) {

    uint16_t wled_speed = (uint16_t)light->effect_speed * 255 / 100;
    uint32_t counter = (now_ms * ((wled_speed >> 2) + 2)) & 0xFFFF;
    uint16_t hue = (uint16_t)((counter >> 8) * 360 / 256);
    uint8_t r, g, b;

    light_hsv_to_rgb(hue, 100, light->val, &r, &g, &b);
    light_addressable_fill(light->addressable_handle, light->channels[0].led_count,
                           light->channels[0].has_white_channel, r, g, b, 0);
}

// 8-bit sine: input 0-255 is one full cycle, output 0-255 centered on 128 (WLED/FastLED sin8).
static uint8_t sin8(uint8_t x) {
    return (uint8_t)(128.0f + 127.0f * sinf((float)x * (2.0f * (float)M_PI) / 256.0f));
}

// Ported 1:1 from WLED util.cpp: attack/hold/decay/hold triangle-ish square wave, x is 0-255.
static int8_t tristate_square8(uint8_t x, uint8_t pulsewidth, uint8_t attdec) {
    int8_t a = 127;
    if (x > 127) {
        a = -127;
        x -= 127;
    }
    if (x < attdec) {
        return (int8_t)((int16_t)x * a / attdec);
    } else if (x < pulsewidth - attdec) {
        return a;
    } else if (x < pulsewidth) {
        return (int8_t)((int16_t)(pulsewidth - x) * a / attdec);
    }
    return 0;
}

static uint8_t qadd8(uint8_t a, uint8_t b) {
    unsigned sum = (unsigned)a + b;
    return sum > 255 ? 255 : (uint8_t)sum;
}

static uint8_t qsub8(uint8_t a, uint8_t b) { return a > b ? (uint8_t)(a - b) : 0; }

// Ported from FastLED HeatColor(): black -> red -> yellow -> white, no palette involved.
static void heat_color(uint8_t temperature, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t t192 = (uint8_t)(((uint16_t)temperature * 191) / 255);
    uint8_t heatramp = (uint8_t)((t192 & 0x3F) << 2);
    if (t192 & 0x80) {
        *r = 255;
        *g = 255;
        *b = heatramp;
    } else if (t192 & 0x40) {
        *r = 255;
        *g = heatramp;
        *b = 0;
    } else {
        *r = heatramp;
        *g = 0;
        *b = 0;
    }
}

// Rescales the 0-100 effect_speed/effect_intensity sliders to WLED's native 0-255 range, so
// ported WLED timing constants (e.g. "(speed>>2)+2") feel the same across the full slider.
static uint16_t wled_scale(uint8_t value_0_100) { return (uint16_t)value_0_100 * 255 / 100; }

// Linear interpolation from `a` (t=0) to `b` (t=255), per 8-bit channel.
static uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t) {
    return (uint8_t)(a + (((int16_t)b - a) * t) / 255);
}

// Renders fx_state.buf (0-255 per pixel: 0 = color2, 255 = main color) as a blend of the two.
// val2 is scaled as a percentage of val, not absolute, so dimming the light dims color2 with
// it (see light_internal.h for why hue2/sat2/val2 aren't inherently "background").
static void render_brightness_background(light_config_t *light) {
    uint16_t led_count =
        light->channels[0].led_count; // addressable lights have exactly one channel
    bool has_white = light->channels[0].has_white_channel;
    const uint8_t *buf = light->fx_state.buf;
    uint16_t buf_led_count =
        led_count > LIGHT_EFFECTS_MAX_LEDS ? LIGHT_EFFECTS_MAX_LEDS : led_count;
    uint8_t r1, g1, b1;
    light_hsv_to_rgb(light->hue, light->sat, light->val, &r1, &g1, &b1);
    uint8_t r2, g2, b2;
    uint8_t val2_scaled = (uint8_t)(((uint16_t)light->val2 * light->val) / 100);
    light_hsv_to_rgb(light->hue2, light->sat2, val2_scaled, &r2, &g2, &b2);
    for (uint16_t i = 0; i < led_count; i++) {
        uint8_t level = i < buf_led_count ? buf[i] : 0;
        uint8_t r = lerp8(r2, r1, level);
        uint8_t g = lerp8(g2, g1, level);
        uint8_t b = lerp8(b2, b1, level);
        light_addressable_set_pixel(light->addressable_handle, i, has_white, r, g, b, 0);
    }
}

// Ported from WLED's mode_washing_machine: waves rotate forward, pause, then reverse.
static void effect_washing_machine(light_config_t *light, uint32_t now_ms) {
    uint16_t led_count = light->channels[0].led_count;
    bool has_white = light->channels[0].has_white_channel;
    int8_t speed = tristate_square8((uint8_t)((now_ms >> 7) & 0xFF), 90, 15);
    uint16_t wled_speed = wled_scale(light->effect_speed);
    int32_t delta = ((int32_t)speed * 2048) / (int32_t)(512 - wled_speed);
    light->fx_state.step += (uint32_t)delta;

    uint8_t br, bg, bb;
    light_hsv_to_rgb(light->hue, light->sat, light->val, &br, &bg, &bb);
    uint8_t density = (uint8_t)(light->effect_intensity / 25 + 1);

    for (uint16_t i = 0; i < led_count; i++) {
        uint8_t phase = (uint8_t)((density * 255u * i / led_count) + (light->fx_state.step >> 7));
        uint8_t col = sin8(phase);
        uint8_t r = (uint8_t)(((uint16_t)br * col) / 255);
        uint8_t g = (uint8_t)(((uint16_t)bg * col) / 255);
        uint8_t b = (uint8_t)(((uint16_t)bb * col) / 255);
        light_addressable_set_pixel(light->addressable_handle, i, has_white, r, g, b, 0);
    }
}

// Ported 1:1 from WLED's mode_android: a colored bar grows, shrinks, and travels around the
// strip. fx_state.aux0 = bar start, aux1 = size<<1|shrinking, step = next update timestamp,
// data0 = free-running counter (WLED SEGENV.aux0/aux1/step/data equivalents).
static void effect_android(light_config_t *light, uint32_t now_ms) {
    uint16_t led_count = light->channels[0].led_count;
    light_effect_state_t *st = &light->fx_state;
    uint16_t size = st->aux1 >> 1;
    bool shrinking = st->aux1 & 0x01;

    if ((int32_t)(now_ms - st->step) >= 0) {
        uint16_t wled_speed = wled_scale(light->effect_speed);
        st->step = now_ms + 3 + ((8u * (uint32_t)(255 - wled_speed)) / led_count);

        uint16_t wled_intensity = wled_scale(light->effect_intensity);
        uint16_t max_size = (uint16_t)((uint32_t)wled_intensity * led_count / 255);
        if (size > max_size) {
            shrinking = true;
        } else if (size < 2) {
            shrinking = false;
        }

        if (!shrinking) {
            if ((st->data0 % 3) == 1) {
                st->aux0++;
            } else {
                size++;
            }
        } else {
            st->aux0++;
            if ((st->data0 % 3) != 1) {
                size--;
            }
        }
        st->aux1 = (uint16_t)((size << 1) | (shrinking ? 1 : 0));
        st->data0++;
        if (st->aux0 >= led_count) {
            st->aux0 = 0;
        }
    }

    // Writes lit/unlit state as a 0/255 buffer so render_brightness_background can draw it.
    uint16_t buf_led_count =
        led_count > LIGHT_EFFECTS_MAX_LEDS ? LIGHT_EFFECTS_MAX_LEDS : led_count;
    uint16_t start = st->aux0;
    uint16_t end = (uint16_t)((st->aux0 + size) % led_count);
    for (uint16_t i = 0; i < buf_led_count; i++) {
        bool lit = (start < end) ? (i >= start && i < end) : (i >= start || i < end);
        st->buf[i] = lit ? 255 : 0;
    }
    render_brightness_background(light);
}

// "Boost" (WLED SEGMENT.custom3) isn't part of the official speed/intensity API (see
// light_internal.h) - fixed at WLED's own default (DEFAULT_C3), tune here in code if needed.
#define FIRE_BOOST 16

// Ported from WLED's mode_fire_2012 (classic FastLED Fire2012, no 2D/virtual-strip handling -
// this project only drives 1D strips). fx_state.buf is the "heat" array, fx_state.step
// throttles the simulation step independently of render FPS (WLED's "it" / SEGENV.step).
static void effect_fire_2012(light_config_t *light, uint32_t now_ms) {
    uint16_t led_count = light->channels[0].led_count;
    bool has_white = light->channels[0].has_white_channel;
    uint8_t *heat = light->fx_state.buf;
    uint16_t buf_led_count =
        led_count > LIGHT_EFFECTS_MAX_LEDS ? LIGHT_EFFECTS_MAX_LEDS : led_count;
    if (led_count > LIGHT_EFFECTS_MAX_LEDS) {
        ESP_LOGW(TAG, "fire_2012: led_count %u exceeds LIGHT_EFFECTS_MAX_LEDS %u, truncating",
                 led_count, (unsigned)LIGHT_EFFECTS_MAX_LEDS);
    }

    uint32_t it = now_ms >> 5;
    if (it != light->fx_state.step) {
        uint16_t wled_speed = wled_scale(light->effect_speed);
        unsigned ignition = buf_led_count / 10 > 3 ? buf_led_count / 10 : 3;

        // Step 1: cool down every cell.
        for (uint16_t i = 0; i < buf_led_count; i++) {
            uint8_t cool =
                (uint8_t)(esp_random() % ((((20 + wled_speed / 3) * 16) / buf_led_count) + 2));
            uint8_t min_temp = (i < ignition) ? (uint8_t)((ignition - i) / 4 + 16) : 0;
            uint8_t temp = qsub8(heat[i], cool);
            heat[i] = temp < min_temp ? min_temp : temp;
        }

        // Step 2: heat drifts "up" the strip.
        for (int k = buf_led_count - 1; k > 1; k--) {
            heat[k] = (uint8_t)((heat[k - 1] + (heat[k - 2] << 1)) / 3);
        }

        // Step 3: random sparks near the base.
        uint16_t wled_intensity = wled_scale(light->effect_intensity);
        if ((esp_random() % 256) <= wled_intensity) {
            unsigned y = esp_random() % ignition;
            uint8_t boost = (uint8_t)((17 + FIRE_BOOST) * (ignition - y / 2) / ignition);
            uint8_t lo = (uint8_t)(96 + 2 * boost);
            uint8_t hi = (uint8_t)(207 + boost);
            uint8_t spark = (uint8_t)(lo + (esp_random() % (unsigned)(hi - lo + 1)));
            heat[y] = qadd8(heat[y], spark);
        }

        light->fx_state.step = it;
    }

    // Step 4: heat -> color. heat_color() itself has no concept of brightness, so scale by
    // light->val afterwards - otherwise the light's brightness slider would do nothing here,
    // unlike every other effect (they all route through light_hsv_to_rgb/light_compute_rgbcw,
    // which do apply val).
    for (uint16_t i = 0; i < led_count; i++) {
        uint8_t r = 0, g = 0, b = 0;
        if (i < buf_led_count) {
            heat_color(heat[i] > 240 ? 240 : heat[i], &r, &g, &b);
            r = (uint8_t)(((uint16_t)r * light->val) / 100);
            g = (uint8_t)(((uint16_t)g * light->val) / 100);
            b = (uint8_t)(((uint16_t)b * light->val) / 100);
        }
        light_addressable_set_pixel(light->addressable_handle, i, has_white, r, g, b, 0);
    }
}

// Inspired by WLED's mode_twinkle (FX.cpp:655-681), not a 1:1 port - WLED avoids a full
// per-pixel buffer (replays a PRNG sequence instead) specifically because it doesn't have one;
// this project already added fx_state.buf for Fire2012, so it's simpler to just track each
// pixel's brightness directly and fade it every frame.
static void effect_twinkle(light_config_t *light, uint32_t now_ms) {
    (void)now_ms;
    uint8_t *buf = light->fx_state.buf;
    uint16_t buf_led_count = light->channels[0].led_count > LIGHT_EFFECTS_MAX_LEDS
                                 ? LIGHT_EFFECTS_MAX_LEDS
                                 : light->channels[0].led_count;

    uint8_t fade_amount = (uint8_t)(4 + light->effect_speed / 4);
    for (uint16_t i = 0; i < buf_led_count; i++) {
        buf[i] = qsub8(buf[i], fade_amount);
    }

    uint8_t spawn_chance = (uint8_t)wled_scale(light->effect_intensity);
    if ((esp_random() % 256) < spawn_chance) {
        buf[esp_random() % buf_led_count] = 255;
    }

    render_brightness_background(light);
}

// Inspired by WLED's mode_meteor (FX.cpp:2378-2441), classic (non-"smooth") variant - skips
// SEGMENT.check1/check3 (booleans outside the speed/intensity "official API", see
// light_internal.h). fx_state.buf is the per-pixel trail brightness (WLED's trail[]),
// fx_state.step accumulates the meteor head's position.
static void effect_meteor(light_config_t *light, uint32_t now_ms) {
    (void)now_ms;
    light_effect_state_t *st = &light->fx_state;
    uint8_t *trail = st->buf;
    uint16_t led_count = light->channels[0].led_count;
    uint16_t buf_led_count =
        led_count > LIGHT_EFFECTS_MAX_LEDS ? LIGHT_EFFECTS_MAX_LEDS : led_count;

    uint16_t wled_speed = wled_scale(light->effect_speed);
    st->step += wled_speed + 1;
    uint16_t head = (uint16_t)(((uint32_t)st->step * buf_led_count) >> 16);

    uint8_t fade_amount = (uint8_t)(4 + (100 - light->effect_intensity) / 5);
    for (uint16_t i = 0; i < buf_led_count; i++) {
        trail[i] = qsub8(trail[i], fade_amount);
    }

    uint16_t meteor_size = buf_led_count / 20;
    if (meteor_size < 1) {
        meteor_size = 1;
    }
    for (uint16_t j = 0; j < meteor_size; j++) {
        trail[(head + j) % buf_led_count] = 255;
    }

    render_brightness_background(light);
}

// Single source of truth per effect: name (for the cmnd "effect" field) + render function,
// indexed by light_effect_t.
static const struct {
    const char *name;
    light_effect_fn_t fn;
} k_effects[] = {
    [LIGHT_EFFECT_NONE] = {"solid", effect_solid},
    [LIGHT_EFFECT_BLINK] = {"blink", effect_blink},
    [LIGHT_EFFECT_BREATHE] = {"breathe", effect_breathe},
    [LIGHT_EFFECT_RAINBOW] = {"rainbow", effect_rainbow},
    [LIGHT_EFFECT_WASHING_MACHINE] = {"washing_machine", effect_washing_machine},
    [LIGHT_EFFECT_ANDROID] = {"android", effect_android},
    [LIGHT_EFFECT_FIRE_2012] = {"fire_2012", effect_fire_2012},
    [LIGHT_EFFECT_TWINKLE] = {"twinkle", effect_twinkle},
    [LIGHT_EFFECT_METEOR] = {"meteor", effect_meteor},
};

bool light_effect_from_name(const char *name, light_effect_t *out) {
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_effects) / sizeof(k_effects[0]); i++) {
        if (strcasecmp(name, k_effects[i].name) == 0) {
            *out = (light_effect_t)i;
            return true;
        }
    }
    return false;
}

size_t light_effect_count(void) { return sizeof(k_effects) / sizeof(k_effects[0]); }

const char *light_effect_name(light_effect_t effect) {
    if ((size_t)effect >= sizeof(k_effects) / sizeof(k_effects[0])) {
        return k_effects[LIGHT_EFFECT_NONE].name;
    }
    return k_effects[effect].name;
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
    if (effect >= sizeof(k_effects) / sizeof(k_effects[0]) || !k_effects[effect].fn) {
        effect = LIGHT_EFFECT_NONE;
    }
    k_effects[effect].fn(light, now_ms);

    led_strip_refresh(light->addressable_handle);
}
