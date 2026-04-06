#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "esp_log.h"

#include "neopixel.h"
#include "neopixel_effects.h"

#define TAG "cikon:neopixel:effects"

#define EFFECTS_TASK_STACK 2048
#define EFFECTS_TASK_PRIORITY 5

typedef struct {
    neopixel_effect_t effect;
    uint32_t color;
    uint8_t speed;
} effect_params_t;

static TaskHandle_t s_effect_task = NULL;
static neopixel_effect_t s_current_effect = NEOPIXEL_EFFECT_NONE;

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
    while (true) {
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
    while (true) {
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
    while (true) {
        for (uint16_t i = 0; i < count; i++) {
            // HSV → RGB, S=1, V=1, hue per pixel offset
            uint16_t h = (hue + i * (360 / count)) % 360;
            uint8_t r, g, b;
            uint8_t hi = h / 60;
            uint8_t f = (h % 60) * 255 / 60;
            switch (hi) {
            case 0:
                r = 255;
                g = f;
                b = 0;
                break;
            case 1:
                r = 255 - f;
                g = 255;
                b = 0;
                break;
            case 2:
                r = 0;
                g = 255;
                b = f;
                break;
            case 3:
                r = 0;
                g = 255 - f;
                b = 255;
                break;
            case 4:
                r = f;
                g = 0;
                b = 255;
                break;
            default:
                r = 255;
                g = 0;
                b = 255 - f;
                break;
            }
            neopixel_set_pixel(i, r, g, b);
        }
        neopixel_show();
        hue = (hue + 5) % 360;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

static void effect_scan_task(void *arg) {
    effect_params_t *p = (effect_params_t *)arg;
    uint8_t r = (p->color >> 16) & 0xFF;
    uint8_t g = (p->color >> 8) & 0xFF;
    uint8_t b = p->color & 0xFF;
    uint32_t delay = speed_to_delay_ms(p->speed);
    uint16_t count = neopixel_get_count();

    int pos = 0;
    int dir = 1;
    while (true) {
        neopixel_clear();
        neopixel_set_pixel(pos, r, g, b);
        neopixel_show();
        pos += dir;
        if (pos >= (int)count) {
            pos = count - 2;
            dir = -1;
        }
        if (pos < 0) {
            pos = 1;
            dir = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

static void effect_matrix_task(void *arg) {
    effect_params_t *p = (effect_params_t *)arg;
    uint32_t delay = speed_to_delay_ms(p->speed);
    uint8_t width = neopixel_get_width();
    uint16_t count = neopixel_get_count();

    if (width <= 1) {
        // Linear fallback: scan in green
        while (true) {
            for (uint16_t i = 0; i < count; i++) {
                neopixel_clear();
                neopixel_set_pixel(i, 0, 255, 0);
                neopixel_show();
                vTaskDelay(pdMS_TO_TICKS(delay));
            }
        }
    }

    uint8_t height = count / width;
    // Each column has a "drop" at a random row, trailing fade
    uint8_t drops[32] = {0}; // max 32 columns
    if (width > 32)
        width = 32;

    for (uint8_t c = 0; c < width; c++) {
        drops[c] = (uint8_t)(esp_random() % height);
    }

    while (true) {
        // Fade all pixels
        for (uint8_t y = 0; y < height; y++) {
            for (uint8_t x = 0; x < width; x++) {
                neopixel_set_xy(x, y, 0, 30, 0);
            }
        }
        // Draw bright heads
        for (uint8_t x = 0; x < width; x++) {
            neopixel_set_xy(x, drops[x], 0, 255, 0);
            drops[x] = (drops[x] + 1) % height;
        }
        neopixel_show();
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

static void effect_runner_task(void *arg) {
    effect_params_t *p = (effect_params_t *)arg;

    switch (p->effect) {
    case NEOPIXEL_EFFECT_SOLID:
        effect_solid(p->color);
        break;
    case NEOPIXEL_EFFECT_BLINK:
        effect_blink_task(arg);
        break;
    case NEOPIXEL_EFFECT_PULSE:
        effect_pulse_task(arg);
        break;
    case NEOPIXEL_EFFECT_RAINBOW:
        effect_rainbow_task(arg);
        break;
    case NEOPIXEL_EFFECT_SCAN:
        effect_scan_task(arg);
        break;
    case NEOPIXEL_EFFECT_MATRIX:
        effect_matrix_task(arg);
        break;
    default:
        break;
    }

    free(p);
    s_effect_task = NULL;
    vTaskDelete(NULL);
}

void neopixel_effect_start(neopixel_effect_t effect, uint32_t color, uint8_t speed) {
    neopixel_effect_stop();

    s_current_effect = effect;

    if (effect == NEOPIXEL_EFFECT_NONE) {
        return;
    }

    effect_params_t *p = malloc(sizeof(effect_params_t));
    if (!p) {
        ESP_LOGE(TAG, "Failed to allocate effect params");
        return;
    }
    p->effect = effect;
    p->color = color;
    p->speed = speed;

    xTaskCreate(effect_runner_task, "neopixel_fx", EFFECTS_TASK_STACK, p, EFFECTS_TASK_PRIORITY,
                &s_effect_task);
}

void neopixel_effect_stop(void) {
    if (s_effect_task) {
        vTaskDelete(s_effect_task);
        s_effect_task = NULL;
    }
    s_current_effect = NEOPIXEL_EFFECT_NONE;
    neopixel_clear();
    neopixel_show();
}

neopixel_effect_t neopixel_effect_get_current(void) { return s_current_effect; }
