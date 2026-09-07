#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "soc/gpio_num.h"
// IWYU pragma: keep - SOC_RMT_SUPPORT_DMA is used in an #if, which IWYU can't see; dropping
// this header would silently take every strip down the SPI path.
#include "soc/soc_caps.h"

#if CONFIG_LIGHT_PERSIST_STATE
#include "nvs.h"
#endif

#include "cJSON.h"

#include "cmnd.h"
#include "json_parser.h"
#include "light_adapter.h"
#include "light_internal.h"
#include "metadata.h"
#include "supervisor.h"
#include "tele.h"

#ifdef LIGHT_HAS_ADDRESSABLE
#include "led_strip.h"
#endif

#if LIGHT_EFFECTS_BUILD
#include "light_effects.h"
#endif

#define TAG "cikon:adapter:light"
#define LIGHT_KELVIN_MIN 2200
#define LIGHT_KELVIN_MAX 7000

#if CONFIG_LIGHT_PERSIST_STATE
// "on" is persisted but only conditionally restored (light_should_restore_on) - see
// light_restore_state.
typedef struct __attribute__((packed)) {
    uint8_t on;
    uint8_t color_mode;
    uint8_t val;
    uint8_t sat;
    uint16_t hue;
    uint16_t cct;
} light_persist_entry_t;

#define LIGHT_NVS_NAMESPACE "light_state"

static light_persist_entry_t last_saved_state[CONFIG_LIGHT_MAX_COUNT];
static bool state_dirty = false;
static uint32_t light_config_fingerprint_cached;
#endif

light_config_t lights[CONFIG_LIGHT_MAX_COUNT + 1]; // +1 sentinel
static bool light_initialized = false;

static bool light_has_role(light_config_t *light, light_channel_role_t role) {
    for (uint8_t i = 0; i < light->channel_count; i++) {
        if (light->channels[i].role == role) {
            return true;
        }
    }
    return false;
}

static int8_t light_find_by_name(const char *name) {
    if (!name) {
        return -1;
    }
    for (int i = 0; i < CONFIG_LIGHT_MAX_COUNT; i++) {
        if (lights[i].channel_count == 0) {
            break;
        }
        if (strcmp(lights[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

#ifdef LIGHT_HAS_ADDRESSABLE
// Pushes one finished color - gamma-corrected and dimmed already - into the strip buffer,
// dispatching to the has_white-aware led_strip call. w is ignored when has_white is false.
static void light_strip_write(led_strip_handle_t handle, uint16_t i, bool has_white,
                              light_rgbcw_t levels) {
    if (has_white) {
        led_strip_set_pixel_rgbw(handle, i, levels.r, levels.g, levels.b, levels.w);
    } else {
        led_strip_set_pixel(handle, i, levels.r, levels.g, levels.b);
    }
}

// Sets one pixel from a raw (full-output, pre-gamma) color: the output stage runs here, so
// effects can work in plain sRGB and never think about gamma or the brightness slider.
// Non-static: shared with light_effects.c.
void light_addressable_set_pixel(led_strip_handle_t handle, uint16_t i, bool has_white,
                                 light_rgbcw_t color, uint8_t brightness) {
    light_strip_write(handle, i, has_white, light_color_to_levels(color, brightness));
}

// led_strip has no "fill all pixels" of its own - only per-pixel set_pixel/set_pixel_rgbw. The
// output stage runs once here rather than per pixel, since every pixel gets the same color.
void light_addressable_fill(led_strip_handle_t handle, uint16_t count, bool has_white,
                            light_rgbcw_t color, uint8_t brightness) {
    light_rgbcw_t levels = light_color_to_levels(color, brightness);
    for (uint16_t i = 0; i < count; i++) {
        light_strip_write(handle, i, has_white, levels);
    }
}
#endif

// The light's color at full output - plain sRGB, before gamma and before the brightness slider,
// both of which light_color_to_levels() applies where the value meets the hardware. Non-static:
// light_effects.c renders the solid (effect == NONE) case from this same source of truth.
light_rgbcw_t light_compute_color(light_config_t *light) {
    light_rgbcw_t color = {0};

    if (!light->on) {
        return color;
    }

    if (light->color_mode && light->has_color) {
        light_color_hsv_to_rgb(light->hue, light->sat, 100, &color.r, &color.g, &color.b);
    } else if (light->has_white) {
        if (light->has_cct) {
            // cct is the cool share, 0-100: splits full output between the two white channels.
            color.c = (uint8_t)(255 * light->cct / 100);
            color.w = (uint8_t)(255 - color.c);
        } else {
            color.c = color.w = 255;
        }
    }
    return color;
}

static void light_write_channels(light_config_t *light) {
    light_rgbcw_t color = light_compute_color(light);
    light_rgbcw_t levels = light_color_to_levels(color, light->val);

    for (uint8_t i = 0; i < light->channel_count; i++) {
        if (light->channels[i].role == CH_SWITCH) {
            gpio_set_level(light->channels[i].gpio,
                           light->on == light->channels[i].active_level ? 1 : 0);
            continue;
        }

#ifdef LIGHT_HAS_ADDRESSABLE
        if (light->channels[i].role == CH_ADDRESSABLE) {
#if LIGHT_EFFECTS_BUILD
            // Rendering is owned exclusively by the effects task (single writer to
            // led_strip_handle_t - see light_effects.c). This function only updates state;
            // callers (light_apply/light_set_state) must call light_effects_notify() to
            // actually push a change to the strip - the task's own boot-time initial render
            // covers the very first state before it's ever notified.
#else
            if (light->addressable_handle) {
                // The raw color and the level, not `levels` - the strip path runs the output
                // stage itself, once, inside the fill.
                light_addressable_fill(light->addressable_handle, light->channels[i].led_count,
                                       light->channels[i].has_white_channel, color, light->val);
                led_strip_refresh(light->addressable_handle);
            }
#endif
            continue;
        }
#endif

        // `levels` is finished: gamma-corrected and dimmed, ready to be an 8-bit LEDC duty.
        uint8_t value = 0;
        switch (light->channels[i].role) {
        case CH_RED:
            value = levels.r;
            break;
        case CH_GREEN:
            value = levels.g;
            break;
        case CH_BLUE:
            value = levels.b;
            break;
        case CH_COLD_WHITE:
            value = levels.c;
            break;
        case CH_WARM_WHITE:
            value = levels.w;
            break;
        default:
            break;
        }
#if CONFIG_LIGHT_ENABLE_FADE
        ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, light->channels[i].ledc_ch, value,
                                CONFIG_LIGHT_FADE_TIME_MS);
        ledc_fade_start(LEDC_LOW_SPEED_MODE, light->channels[i].ledc_ch, LEDC_FADE_NO_WAIT);
#else
        ledc_set_duty(LEDC_LOW_SPEED_MODE, light->channels[i].ledc_ch, value);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, light->channels[i].ledc_ch);
#endif
    }
}

// Clamps to [min_val, max_val]. The main light's brightness (v) calls this with min_val=1:
// "on" true with every channel at 0 is an ambiguous state a slider dragged to the bottom can
// trigger, so it's floored just above off. Everything else (effect_speed/effect_intensity/
// val2/...) has no such ambiguity - 0 is a valid, meaningful value - and uses min_val=0.
static uint8_t light_clamp_range(int value, uint8_t min_val, uint8_t max_val) {
    if (value < min_val) {
        return min_val;
    }
    if (value > max_val) {
        return max_val;
    }
    return (uint8_t)value;
}

static void light_apply(light_config_t *light, const char *args_json_str) {
    cJSON *root = cJSON_Parse(args_json_str);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON: %s", args_json_str);
        return;
    }

    if (cJSON_IsObject(root)) {
        cJSON *h = cJSON_GetObjectItem(root, "h");
        cJSON *s = cJSON_GetObjectItem(root, "s");
        cJSON *v = cJSON_GetObjectItem(root, "v");
        cJSON *cct = cJSON_GetObjectItem(root, "cct");
        cJSON *on = cJSON_GetObjectItem(root, "on");

        if (cct && light->has_white) {
            light->cct = (uint16_t)cct->valueint;
            light->color_mode = false;
            if (v) {
                light->val = light_clamp_range(v->valueint, 1, 100);
            }
        } else if (h || s) {
            if (h) {
                light->hue = (uint16_t)h->valueint;
            }
            if (s) {
                light->sat = (uint8_t)s->valueint;
            }
            if (v) {
                light->val = light_clamp_range(v->valueint, 1, 100);
            }
            light->color_mode = true;
        } else if (v) {
            light->val = light_clamp_range(v->valueint, 1, 100);
        }

        if (on) {
            light->on = cJSON_IsTrue(on);
        } else if (h || s || v || (cct && light->has_white)) {
            light->on = true;
        }

#if LIGHT_EFFECTS_BUILD
        if (light->is_addressable) {
            cJSON *effect = cJSON_GetObjectItem(root, "effect");
            cJSON *speed = cJSON_GetObjectItem(root, "speed");
            cJSON *intensity = cJSON_GetObjectItem(root, "intensity");
            cJSON *h2 = cJSON_GetObjectItem(root, "h2");
            cJSON *s2 = cJSON_GetObjectItem(root, "s2");
            cJSON *v2 = cJSON_GetObjectItem(root, "v2");
            if (effect && cJSON_IsString(effect)) {
                light_effect_t new_effect;
                if (light_effect_from_name(effect->valuestring, &new_effect)) {
                    if ((uint8_t)new_effect != light->effect) {
                        // Prevents stale state (e.g. android's bar position, fire's heat
                        // array) from leaking into whichever effect gets picked next.
                        memset(&light->fx_state, 0, sizeof(light->fx_state));
                    }
                    light->effect = (uint8_t)new_effect;
                } else {
                    ESP_LOGW(TAG, "Unknown effect '%s'", effect->valuestring);
                }
            }
            if (speed) {
                light->effect_speed = light_clamp_range(speed->valueint, 0, 100);
            }
            if (intensity) {
                light->effect_intensity = light_clamp_range(intensity->valueint, 0, 100);
            }
            if (h2) {
                light->hue2 = (uint16_t)h2->valueint;
            }
            if (s2) {
                light->sat2 = (uint8_t)s2->valueint;
            }
            if (v2) {
                light->val2 = light_clamp_range(v2->valueint, 0, 100);
            }
        }
#endif
    } else {
        logic_state_t state = json_str_as_logic_state(args_json_str);
        light->on = (state == STATE_TOGGLE) ? !light->on : (state == STATE_ON);
    }

    cJSON_Delete(root);

    light_write_channels(light);
#if LIGHT_EFFECTS_BUILD
    if (light->is_addressable) {
        light_effects_notify();
    }
#endif
#if CONFIG_LIGHT_PERSIST_STATE
    state_dirty = true;
#endif
}

// One cmnd is registered per configured light, each pointing at its own trampoline below -
// command_handler_t carries no context, so a single shared handler can't tell which light it
// was called for. LIGHT_CMND_LIST (injected by CMakeLists.txt, sized to match
// LIGHT_GPIO_LIST) generates exactly as many trampolines as there are configured lights.
#ifndef LIGHT_CMND_LIST
#define LIGHT_CMND_LIST // Fallback if CMake didn't inject
#endif

#define X(n)                                                                                       \
    static void light_cmnd_##n(const char *args_json_str) {                                        \
        light_apply(&lights[n], args_json_str);                                                    \
    }
LIGHT_CMND_LIST
#undef X

#define X(n) light_cmnd_##n,
static const command_handler_t light_cmnd_trampolines[] = {LIGHT_CMND_LIST};
#undef X

#if CONFIG_LIGHT_PERSIST_STATE
// Local hash (FNV-1a) - CONFIG_LIGHT_GPIO_LIST is a fixed string at build time, so this only
// needs to run once per boot; the result is cached in light_config_fingerprint_cached.
static uint32_t light_config_fingerprint(void) {
    const char *cursor = CONFIG_LIGHT_GPIO_LIST;
    uint32_t hash = 2166136261u;
    while (*cursor) {
        hash ^= (uint8_t)(*cursor++);
        hash *= 16777619u;
    }
    return hash;
}

// Only restore on/off after a restart we triggered ourselves (cmnd restart, OTA, resetconf -
// all go through esp_safe_restart() -> esp_restart(), which reports as ESP_RST_SW on the next
// boot). Any other reset reason (power loss, brownout, panic, watchdog) leaves lights off,
// matching ESPHome's RESTORE_AND_OFF - color/brightness are restored either way, just not the
// on/off state itself.
static bool light_should_restore_on(void) { return esp_reset_reason() == ESP_RST_SW; }

static void light_save_state(void) {
    light_persist_entry_t current[CONFIG_LIGHT_MAX_COUNT] = {0};
    for (int i = 0; lights[i].channel_count != 0; i++) {
        current[i].on = lights[i].on;
        current[i].color_mode = lights[i].color_mode;
        current[i].val = lights[i].val;
        current[i].sat = lights[i].sat;
        current[i].hue = lights[i].hue;
        current[i].cct = lights[i].cct;
    }

    if (memcmp(current, last_saved_state, sizeof(current)) == 0) {
        return;
    }

    nvs_handle_t handle;
    if (nvs_open(LIGHT_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for light state save");
        return;
    }
    if (nvs_set_blob(handle, "state", current, sizeof(current)) == ESP_OK &&
        nvs_commit(handle) == ESP_OK) {
        memcpy(last_saved_state, current, sizeof(current));
        state_dirty = false;
        ESP_LOGI(TAG, "Light state saved to NVS");
    } else {
        ESP_LOGW(TAG, "Failed to save light state to NVS");
    }
    nvs_close(handle);
}

// Must run before any LEDC/GPIO output setup below, so lights snap straight to their
// restored state instead of flashing defaults first (same reasoning as switch.c's restore
// ordering comment).
static void light_restore_state(void) {
    nvs_handle_t handle;
    if (nvs_open(LIGHT_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for light state restore");
        return;
    }

    uint32_t saved_fingerprint = 0;
    esp_err_t fingerprint_err = nvs_get_u32(handle, "fingerprint", &saved_fingerprint);
    if (fingerprint_err != ESP_OK || saved_fingerprint != light_config_fingerprint_cached) {
        nvs_set_u32(handle, "fingerprint", light_config_fingerprint_cached);
        nvs_commit(handle);
        ESP_LOGI(TAG, "Light config changed or first boot, discarding saved light state");
        nvs_close(handle);
        return;
    }

    light_persist_entry_t saved[CONFIG_LIGHT_MAX_COUNT] = {0};
    size_t size = sizeof(saved);
    if (nvs_get_blob(handle, "state", saved, &size) == ESP_OK) {
        bool restore_on = light_should_restore_on();
        for (int i = 0; lights[i].channel_count != 0; i++) {
            lights[i].color_mode = saved[i].color_mode;
            lights[i].val = saved[i].val;
            lights[i].sat = saved[i].sat;
            lights[i].hue = saved[i].hue;
            lights[i].cct = saved[i].cct;
            if (restore_on) {
                lights[i].on = saved[i].on;
            }
        }
        memcpy(last_saved_state, saved, sizeof(saved));
        ESP_LOGI(TAG, "Light state restored from NVS (on/off %s)",
                 restore_on ? "restored" : "left off");
    }
    nvs_close(handle);
}

#endif

static void light_adapter_on_interval(supervisor_interval_stage_t stage) {
#if CONFIG_LIGHT_PERSIST_STATE
    if (stage == SUPERVISOR_INTERVAL_10S && state_dirty) {
        light_save_state();
    }
#endif
    (void)stage;
}

static esp_err_t light_adapter_init(void) {

    ESP_LOGI(TAG, "Initializing light adapter");

    if (light_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    light_color_init();
    light_config_parse();

#if CONFIG_LIGHT_PERSIST_STATE
    light_config_fingerprint_cached = light_config_fingerprint();
    light_restore_state();
#endif

    ledc_timer_config_t timer_config = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                        .duty_resolution = LEDC_TIMER_8_BIT,
                                        .timer_num = LEDC_TIMER_0,
                                        .freq_hz = CONFIG_LIGHT_PWM_FREQUENCY,
                                        .clk_cfg = LEDC_AUTO_CLK};

    if (ledc_timer_config(&timer_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC timer");
        return ESP_FAIL;
    }

#if CONFIG_LIGHT_ENABLE_FADE
    ledc_fade_func_install(0);
#endif

    size_t trampoline_count = sizeof(light_cmnd_trampolines) / sizeof(light_cmnd_trampolines[0]);

    for (int i = 0; lights[i].channel_count != 0; i++) {
        light_config_t *light = &lights[i];

        for (uint8_t c = 0; c < light->channel_count; c++) {
            if (light->channels[c].role == CH_SWITCH) {
                gpio_num_t gpio = light->channels[c].gpio;
                if (gpio_reset_pin(gpio) != ESP_OK ||
                    gpio_set_direction(gpio, GPIO_MODE_OUTPUT) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to configure GPIO %d for light '%s'", gpio, light->name);
                }
                gpio_set_level(gpio, light->on == light->channels[c].active_level ? 1 : 0);
                continue;
            }

#ifdef LIGHT_HAS_ADDRESSABLE
            if (light->channels[c].role == CH_ADDRESSABLE) {
                led_strip_config_t strip_config = {
                    .strip_gpio_num = light->channels[c].gpio,
                    .max_leds = light->channels[c].led_count,
                    .led_model =
                        light->channels[c].has_white_channel ? LED_MODEL_SK6812 : LED_MODEL_WS2812,
                    .color_component_format = light->channels[c].led_color_format,
                    .flags = {.invert_out = false},
                };
                // Prefer RMT+DMA where the SoC supports it (S3/C3/C6/H2/P4): frees the SPI bus
                // entirely for other peripherals (e.g. W5500 ethernet on P4/S3), and gives the
                // same DMA-backed reliability as SPI without touching a scarce, single-device
                // bus. Classic ESP32/S2 have no RMT+DMA (SOC_RMT_SUPPORT_DMA is undefined for
                // them - confirmed in soc_caps.h), so their tiny RMT hardware memory block needs
                // interrupt-driven refills that WiFi can delay, corrupting colors - SPI+DMA is
                // the fallback there: real DMA on every ESP32 variant, whole frame streams via
                // one hardware transfer, no per-refresh CPU-timing dependency. Only the MOSI
                // line is used - clockless mode.
                //
                // NOTE: the RMT+DMA branch is untested on real hardware (only neocikon, a
                // classic ESP32 using the SPI branch, has been verified end-to-end so far).
                // Test on an actual S3/C3/C6/H2/P4 device with an addressable strip before
                // relying on it.
#if SOC_RMT_SUPPORT_DMA
                led_strip_rmt_config_t rmt_config = {.clk_src = RMT_CLK_SRC_DEFAULT,
                                                     .flags = {.with_dma = true}};
                esp_err_t addressable_err = led_strip_new_rmt_device(&strip_config, &rmt_config,
                                                                     &light->addressable_handle);
#else
                led_strip_spi_config_t spi_config = {.clk_src = SPI_CLK_SRC_DEFAULT,
                                                     .spi_bus = SPI2_HOST,
                                                     .flags = {.with_dma = true}};
                esp_err_t addressable_err = led_strip_new_spi_device(&strip_config, &spi_config,
                                                                     &light->addressable_handle);
#endif
                if (addressable_err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to init addressable strip for light '%s' GPIO %d",
                             light->name, light->channels[c].gpio);
                }
                continue;
            }
#endif

            ledc_channel_config_t ch_config = {.gpio_num = light->channels[c].gpio,
                                               .speed_mode = LEDC_LOW_SPEED_MODE,
                                               .channel = light->channels[c].ledc_ch,
                                               .timer_sel = LEDC_TIMER_0,
                                               .duty = 0,
                                               .hpoint = 0};
            if (ledc_channel_config(&ch_config) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to configure LEDC channel for light '%s' GPIO %d",
                         light->name, light->channels[c].gpio);
            }
        }

        light_write_channels(light);

        if ((size_t)i >= trampoline_count) {
            ESP_LOGE(TAG,
                     "No cmnd trampoline for light '%s' (index %d) - CMake/runtime light "
                     "count mismatch",
                     light->name, i);
            break;
        }
        const char *description;
        if (light->is_switch) {
            description = "Set switch state (on/off/toggle)";
        } else if (light->has_color && light->has_cct) {
            description = "Set light color/CCT/brightness/state ({h,s,v,cct,on} or on/off/toggle)";
        } else if (light->has_color) {
            description = "Set light color/brightness/state ({h,s,v,on} or on/off/toggle)";
        } else if (light->has_cct) {
            description = "Set light CCT/brightness/state ({cct,v,on} or on/off/toggle)";
        } else {
            description = "Set light brightness/state ({v,on} or on/off/toggle)";
        }
        cmnd_register(light->name, description, light_cmnd_trampolines[i]);
    }

#if LIGHT_EFFECTS_BUILD
    // Must run after the loop above, since it needs every addressable_handle already created.
    light_effects_task_start();
#endif

    light_initialized = true;
    ESP_LOGI(TAG, "Light adapter initialized");
    return ESP_OK;
}

static esp_err_t light_adapter_shutdown(void) {
    if (!light_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

#if LIGHT_EFFECTS_BUILD
    // Must run before the loop below deletes addressable_handle values, so the task never
    // touches a freed handle.
    light_effects_task_stop();
#endif

#if CONFIG_LIGHT_PERSIST_STATE
    light_save_state();
#endif

    for (int i = 0; lights[i].channel_count != 0; i++) {
        cmnd_unregister(lights[i].name);
#ifdef LIGHT_HAS_ADDRESSABLE
        if (lights[i].is_addressable && lights[i].addressable_handle) {
            led_strip_del(lights[i].addressable_handle);
            lights[i].addressable_handle = NULL;
        }
#endif
    }

#if CONFIG_LIGHT_ENABLE_FADE
    ledc_fade_func_uninstall();
#endif
    light_initialized = false;
    ESP_LOGI(TAG, "Light adapter shutdown");
    return ESP_OK;
}

void light_set_state(const char *name, bool on) {
    int8_t idx = light_find_by_name(name);
    if (idx < 0) {
        ESP_LOGW(TAG, "Light '%s' not found", name ? name : "(null)");
        return;
    }
    lights[idx].on = on;
    light_write_channels(&lights[idx]);
#if LIGHT_EFFECTS_BUILD
    if (lights[idx].is_addressable) {
        light_effects_notify();
    }
#endif
#if CONFIG_LIGHT_PERSIST_STATE
    state_dirty = true;
#endif
}

bool light_get_state(const char *name) {
    int8_t idx = light_find_by_name(name);
    if (idx < 0) {
        return false;
    }
    return lights[idx].on;
}

static void tele_light(const char *tele_id, cJSON *json_root) {
    (void)tele_id;

    // "lights" lists the names of the flat per-light objects below, so a UI polling /tele
    // can tell which top-level keys are lights without the state itself being nested.
    cJSON *names = cJSON_CreateArray();

    for (int i = 0; lights[i].channel_count != 0; i++) {
        light_config_t *light = &lights[i];
        cJSON *obj = cJSON_CreateObject();

        cJSON_AddBoolToObject(obj, "on", light->on);
        if (!light->is_switch) {
            cJSON_AddNumberToObject(obj, "v", light->val);
        }

#if LIGHT_EFFECTS_BUILD
        if (light->is_addressable) {
            cJSON_AddStringToObject(obj, "effect",
                                    light_effect_name((light_effect_t)light->effect));
        }
#endif

        if (light->has_cct) {
            cJSON_AddNumberToObject(obj, "cct", light->cct);
        }

        if (light->has_color) {
            uint8_t r, g, b;
            if (light->color_mode) {
                // At full value, not scaled by "v" - brightness is reported separately (and HA
                // reads it from there), so folding it in here only costs precision. Scaled, a
                // saturated color quantizes to something with the wrong hue near the bottom of
                // the slider - FF6E54 at v=1 lands on (3,1,1), which reads back as pure red -
                // and HA would then show, and remember, that wrong color.
                light_color_hsv_to_rgb(light->hue, light->sat, 100, &r, &g, &b);
            } else {
                // Not RGBW/RGBCW's actual output (that's computed separately in
                // light_compute_color) - just an approximate warm<->cool tint from cct, so
                // HA's MQTT "template" schema (no color_mode field, unlike "json" schema)
                // doesn't keep painting its color-derived UI (e.g. the brightness slider)
                // with the stale last color while white mode is active. Harmless without HA
                // too - tele publishes unconditionally, this is just an unread field then.
                uint16_t pct = light->has_cct ? light->cct : 50;
                r = (uint8_t)((255 * (100 - pct) + 220 * pct) / 100);
                g = (uint8_t)((180 * (100 - pct) + 230 * pct) / 100);
                b = (uint8_t)((107 * (100 - pct) + 255 * pct) / 100);
            }
            cJSON_AddNumberToObject(obj, "r", r);
            cJSON_AddNumberToObject(obj, "g", g);
            cJSON_AddNumberToObject(obj, "b", b);
        }

        // Raw per-channel values (alongside cct/r/g/b above, not instead of - HA's
        // color_temp_template still reads cct) so a simple UI can render one control per
        // physical channel just by checking which keys are present, with no capability
        // flags to interpret: has "c" -> cold-white button, has "w" -> warm-white button.
        bool has_c = light_has_role(light, CH_COLD_WHITE);
        bool has_w = light_has_role(light, CH_WARM_WHITE);
        if (has_c || has_w) {
            uint8_t c_val = 0, w_val = 0;
            if (!light->color_mode || !light->has_color) {
                // Logical (pre-gamma) levels, same convention as the r/g/b reported above: the
                // cold/warm split at full output, since "v" carries the level on its own.
                light_rgbcw_t color = light_compute_color(light);
                c_val = color.c;
                w_val = color.w;
            }
            if (has_c) {
                cJSON_AddNumberToObject(obj, "c", c_val);
            }
            if (has_w) {
                cJSON_AddNumberToObject(obj, "w", w_val);
            }
        }

        cJSON_AddItemToObject(json_root, light->name, obj);
        cJSON_AddItemToArray(names, cJSON_CreateString(light->name));
    }

    cJSON_AddItemToObject(json_root, "lights", names);
}

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
#ifndef HA_ENTITY_LIST
#define HA_ENTITY_LIST // Fallback if CMake didn't inject
#endif

static void light_ha_build(cJSON *payload, const char *sanitized_name) {
    int8_t idx = light_find_by_name(sanitized_name);
    char cmd_buf[400];
    char buf[192];

    cJSON_AddStringToObject(payload, "schema", "template");

    snprintf(cmd_buf, sizeof(cmd_buf),
             "{\"%s\":{ "
             "{%% if hue is defined %%}\"h\":{{ hue }},{%% endif %%}"
             "{%% if sat is defined %%}\"s\":{{ sat }},{%% endif %%}"
             "{%% if brightness is defined %%}\"v\":{{ (brightness / 255 * 100) | round }},{%% "
             "endif %%}"
             "{%% if color_temp is defined %%}\"cct\":{{ ((color_temp - %d) / (%d - %d) * 100) | "
             "round }},{%% endif %%}"
             "{%% if effect is defined %%}\"effect\":\"{{ effect }}\",{%% endif %%}"
             "\"on\":true}}",
             sanitized_name, LIGHT_KELVIN_MIN, LIGHT_KELVIN_MAX, LIGHT_KELVIN_MIN);

    cJSON_AddStringToObject(payload, "command_on_template", cmd_buf);

    snprintf(buf, sizeof(buf), "{\"%s\":{\"on\":false}}", sanitized_name);
    cJSON_AddStringToObject(payload, "command_off_template", buf);

    snprintf(buf, sizeof(buf), "{%% if value_json.%s.on %%}on{%% else %%}off{%% endif %%}",
             sanitized_name);
    cJSON_AddStringToObject(payload, "state_template", buf);

    if (idx >= 0 && !lights[idx].is_switch) {
        snprintf(buf, sizeof(buf), "{{ (value_json.%s.v / 100 * 255) | round }}", sanitized_name);
        cJSON_AddStringToObject(payload, "brightness_template", buf);
    }

    if (idx >= 0 && lights[idx].has_color) {
        snprintf(buf, sizeof(buf), "{{ value_json.%s.r }}", sanitized_name);
        cJSON_AddStringToObject(payload, "red_template", buf);
        snprintf(buf, sizeof(buf), "{{ value_json.%s.g }}", sanitized_name);
        cJSON_AddStringToObject(payload, "green_template", buf);
        snprintf(buf, sizeof(buf), "{{ value_json.%s.b }}", sanitized_name);
        cJSON_AddStringToObject(payload, "blue_template", buf);
    }

    if (idx >= 0 && lights[idx].has_cct) {
        cJSON_AddBoolToObject(payload, "color_temp_kelvin", true);
        cJSON_AddNumberToObject(payload, "min_kelvin", LIGHT_KELVIN_MIN);
        cJSON_AddNumberToObject(payload, "max_kelvin", LIGHT_KELVIN_MAX);
        snprintf(buf, sizeof(buf), "{{ (value_json.%s.cct / 100 * (%d - %d) + %d) | round }}",
                 sanitized_name, LIGHT_KELVIN_MAX, LIGHT_KELVIN_MIN, LIGHT_KELVIN_MIN);
        cJSON_AddStringToObject(payload, "color_temp_template", buf);
    }

#if LIGHT_EFFECTS_BUILD
    if (idx >= 0 && lights[idx].is_addressable) {
        cJSON *effect_list = cJSON_CreateArray();
        for (size_t i = 0; i < light_effect_count(); i++) {
            cJSON_AddItemToArray(effect_list,
                                 cJSON_CreateString(light_effect_name((light_effect_t)i)));
        }
        cJSON_AddItemToObject(payload, "effect_list", effect_list);

        snprintf(buf, sizeof(buf), "{{ value_json.%s.effect }}", sanitized_name);
        cJSON_AddStringToObject(payload, "effect_template", buf);
    }
#endif

    cJSON_DeleteItemFromObject(payload, "val_tpl");
}

#define HA_ENTITY_ENTRY(light_name)                                                                \
    {.type = HA_LIGHT, .name = light_name, .custom_builder = light_ha_build},

static const ha_metadata_t light_ha_metadata = {
    .magic = HA_METADATA_MAGIC, .entities = {HA_ENTITY_LIST{.type = HA_ENTITY_NONE}}};
#undef HA_ENTITY_ENTRY
#endif

supervisor_platform_adapter_t light_adapter = {
    .name = "light",
    .init = light_adapter_init,
    .shutdown = light_adapter_shutdown,
    .on_interval = light_adapter_on_interval,
    .tele_group = (const tele_entry_t[]){{"light", tele_light}, {NULL, NULL}},
    .cmnd_group = NULL, // registered dynamically per light in light_adapter_init
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    .metadata = &light_ha_metadata,
#endif
};
