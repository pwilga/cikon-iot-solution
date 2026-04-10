#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "neopixel.h"
#include "neopixel_colors.h"
#include "neopixel_effects.h"

#define TAG "cikon:neopixel:effects"

typedef struct {
    neopixel_effect_t effect;
    uint32_t color;
    uint8_t speed;
} effect_params_t;

static TaskHandle_t s_effect_task = NULL;
static neopixel_effect_t s_current_effect = NEOPIXEL_EFFECT_NONE;
static effect_params_t s_effect_params;
static volatile bool s_stop_requested = false; // Graceful shutdown flag

// speed 1-10 → delay in ms (10=fast=50ms, 1=slow=500ms)
static uint32_t speed_to_delay_ms(uint8_t speed) {
    if (speed < 1)
        speed = 1;
    if (speed > 10)
        speed = 10;
    return 550 - (speed * 50);
}

static void effect_solid(uint32_t color) {
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    neopixel_fill(r, g, b);
    neopixel_show();
}

static void effect_blink_task(void *arg) {
    effect_params_t *p = (effect_params_t *)arg;
    uint8_t r = (p->color >> 16) & 0xFF;
    uint8_t g = (p->color >> 8) & 0xFF;
    uint8_t b = p->color & 0xFF;
    uint32_t delay = speed_to_delay_ms(p->speed);

    bool on = true;
    while (!s_stop_requested) {
        if (on) {
            neopixel_fill(r, g, b);
        } else {
            neopixel_clear();
        }
        neopixel_show();
        on = !on;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

static void effect_pulse_task(void *arg) {
    effect_params_t *p = (effect_params_t *)arg;
    uint8_t r = (p->color >> 16) & 0xFF;
    uint8_t g = (p->color >> 8) & 0xFF;
    uint8_t b = p->color & 0xFF;
    uint32_t delay = speed_to_delay_ms(p->speed) / 10;

    int step = 0;
    int dir = 1;
    while (!s_stop_requested) {
        float factor = (float)step / 255.0f;
        neopixel_fill((uint8_t)(r * factor), (uint8_t)(g * factor), (uint8_t)(b * factor));
        neopixel_show();
        step += dir * 5;
        if (step >= 255) {
            step = 255;
            dir = -1;
        }
        if (step <= 0) {
            step = 0;
            dir = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(delay > 0 ? delay : 1));
    }
}

static void effect_rainbow_task(void *arg) {
    effect_params_t *p = (effect_params_t *)arg;
    uint32_t delay = speed_to_delay_ms(p->speed);
    uint16_t count = neopixel_get_count();

    uint16_t hue = 0;
    while (!s_stop_requested) {
        for (uint16_t i = 0; i < count; i++) {
            // HSV: każdy LED ma offset hue (dla wielu LEDs robi gradację)
            uint16_t h = (hue + i * (360 / count)) % 360;
            uint32_t color = neopixel_hsv(h, 100, 100);

            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;

            neopixel_set_pixel(i, r, g, b);
        }
        neopixel_show();
        hue = (hue + 5) % 360;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

static void effect_runner_task(void *arg) {
    effect_params_t *p = (effect_params_t *)arg;

    switch (p->effect) {
    case NEOPIXEL_EFFECT_BLINK:
        effect_blink_task(arg);
        break;
    case NEOPIXEL_EFFECT_PULSE:
        effect_pulse_task(arg);
        break;
    case NEOPIXEL_EFFECT_RAINBOW:
        effect_rainbow_task(arg);
        break;
    default:
        break;
    }

    s_effect_task = NULL;
    vTaskDelete(NULL);
}

void neopixel_effect_start(neopixel_effect_t effect, uint32_t color, uint8_t speed) {
    // Rate limiting: max 1 call per 100ms (prevents spam crashes)
    static TickType_t s_last_start = 0;
    TickType_t now = xTaskGetTickCount();
    if ((now - s_last_start) < pdMS_TO_TICKS(100)) {
        return; // Too fast, ignore
    }
    s_last_start = now;

    neopixel_effect_stop();

    s_stop_requested = false; // Clear stop flag for new effect
    s_current_effect = effect;

    if (effect == NEOPIXEL_EFFECT_NONE) {
        return;
    }

    if (effect == NEOPIXEL_EFFECT_SOLID) {
        effect_solid(color);
        return;
    }

    s_effect_params.effect = effect;
    s_effect_params.color = color;
    s_effect_params.speed = speed;

    xTaskCreate(effect_runner_task, "neopixel_fx", CONFIG_NEOPIXEL_EFFECTS_TASK_STACK_SIZE,
                &s_effect_params, CONFIG_NEOPIXEL_EFFECTS_TASK_PRIORITY, &s_effect_task);
}

void neopixel_effect_stop(void) {
    if (s_effect_task) {
        // Request graceful shutdown
        s_stop_requested = true;

        // Wait for task to exit (max 500ms)
        for (int i = 0; i < 50 && s_effect_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // If still alive, force kill (shouldn't happen with delays in effects)
        if (s_effect_task) {
            vTaskDelete(s_effect_task);
            s_effect_task = NULL;
        }
    }
    s_current_effect = NEOPIXEL_EFFECT_NONE;
    neopixel_clear();
    neopixel_show();
}

neopixel_effect_t neopixel_effect_get_current(void) { return s_current_effect; }
