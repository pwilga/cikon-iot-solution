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

#include "light.h"
#include "light_internal.h"

#ifdef LIGHT_HAS_ADDRESSABLE
#include "led_strip.h"
#endif

#if LIGHT_EFFECTS_BUILD
#include "light_effects.h"
#endif

#define TAG "cikon:light"

#if CONFIG_LIGHT_PERSIST_STATE
// "on" is persisted but only conditionally restored (light_should_restore_on) - see
// light_restore_state. Effect settings (effect, speed, intensity, hue2/sat2/val2) are not
// persisted at all, so a strip comes back on "solid" after a reboot.
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

// The effect catalogue lives here, not in light_effects.c: that file is compiled only when the
// effects engine is enabled, and HA discovery asks these questions in every build.
size_t light_effect_count(void) {
#if LIGHT_EFFECTS_BUILD
    return light_effect_table_size;
#else
    return 0;
#endif
}

const char *light_effect_name(size_t effect) {
#if LIGHT_EFFECTS_BUILD
    return effect < light_effect_table_size ? light_effect_table[effect].name : NULL;
#else
    (void)effect;
    return NULL;
#endif
}

// Case-insensitive lookup for the cmnd "effect" field; -1 when the name matches nothing.
int8_t light_effect_index(const char *name) {
#if LIGHT_EFFECTS_BUILD
    if (name) {
        for (size_t i = 0; i < light_effect_table_size; i++) {
            if (strcasecmp(name, light_effect_table[i].name) == 0) {
                return (int8_t)i;
            }
        }
    }
#else
    (void)name;
#endif
    return -1;
}

int8_t light_index_by_name(const char *name) {
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

size_t light_count(void) {
    size_t count = 0;
    while (count < CONFIG_LIGHT_MAX_COUNT && lights[count].channel_count != 0) {
        count++;
    }
    return count;
}

// The one bounds check behind every index-taking entry point below.
static light_config_t *light_by_index(size_t index) {
    return index < light_count() ? &lights[index] : NULL;
}

const char *light_name(size_t index) {
    light_config_t *light = light_by_index(index);
    return light ? light->name : NULL;
}

bool light_get_caps(size_t index, light_caps_t *caps) {
    light_config_t *light = light_by_index(index);
    if (!light || !caps) {
        return false;
    }

    *caps = (light_caps_t){
        .is_switch = light->is_switch,
        .has_color = light->has_color,
        .has_cct = light->has_cct,
        .has_cold_white = light_has_role(light, CH_COLD_WHITE),
        .has_warm_white = light_has_role(light, CH_WARM_WHITE),
#ifdef LIGHT_HAS_ADDRESSABLE
        .has_effects = light->is_addressable && light_effect_count() > 0,
#else
        .has_effects = false,
#endif
    };
    return true;
}

bool light_get_state(size_t index, light_state_t *state) {
    light_config_t *light = light_by_index(index);
    if (!light || !state) {
        return false;
    }

    *state = (light_state_t){
        .on = light->on,
        .brightness = light->val,
        .cct = light->cct,
    };

#ifdef LIGHT_HAS_ADDRESSABLE
    if (light->is_addressable && light_effect_count() > 0) {
        state->effect = light_effect_name(light->effect);
    }
#endif

    if (light->has_color) {
        if (light->color_mode) {
            light_color_hsv_to_rgb(light->hue, light->sat, 100, &state->color.r, &state->color.g,
                                   &state->color.b);
        } else {
            // Not the RGBW/RGBCW output - an approximate warm<->cool tint from cct, so a
            // color-derived UI (HA's "template" schema carries no color_mode) stops painting
            // itself with the stale last color while white mode is active.
            uint16_t pct = light->has_cct ? light->cct : 50;
            state->color.r = (uint8_t)((255 * (100 - pct) + 220 * pct) / 100);
            state->color.g = (uint8_t)((180 * (100 - pct) + 230 * pct) / 100);
            state->color.b = (uint8_t)((107 * (100 - pct) + 255 * pct) / 100);
        }
    }

    if (!light->color_mode || !light->has_color) {
        light_rgbcw_t full = light_compute_color(light);
        state->color.c = full.c;
        state->color.w = full.w;
    }

    return true;
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

esp_err_t light_apply_change(size_t index, const light_state_change_t *change) {
    light_config_t *light = light_by_index(index);
    if (!light) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!change) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t fields = change->fields;
    // cct only means anything on a light that has a white channel to aim it at.
    const bool cct_wins = (fields & LIGHT_FIELD_CCT) && light->has_white;

    if (cct_wins) {
        light->cct = change->cct;
        light->color_mode = false;
    } else if (fields & (LIGHT_FIELD_HUE | LIGHT_FIELD_SATURATION)) {
        if (fields & LIGHT_FIELD_HUE) {
            light->hue = change->hue;
        }
        if (fields & LIGHT_FIELD_SATURATION) {
            light->sat = change->saturation;
        }
        light->color_mode = true;
    }

    if (fields & LIGHT_FIELD_BRIGHTNESS) {
        light->val = light_clamp_range(change->brightness, 1, 100);
    }

    if (fields & LIGHT_FIELD_ON) {
        light->on = change->on;
    } else if (cct_wins || (fields & (LIGHT_FIELD_HUE | LIGHT_FIELD_SATURATION |
                                      LIGHT_FIELD_BRIGHTNESS))) {
        light->on = true;
    }

#if LIGHT_EFFECTS_BUILD
    if (light->is_addressable) {
        if ((fields & LIGHT_FIELD_EFFECT) && change->effect) {
            int8_t new_effect = light_effect_index(change->effect);
            if (new_effect >= 0) {
                if ((uint8_t)new_effect != light->effect) {
                    // Prevents stale state (e.g. android's bar position, fire's heat array)
                    // from leaking into whichever effect gets picked next.
                    memset(&light->fx_state, 0, sizeof(light->fx_state));
                }
                light->effect = (uint8_t)new_effect;
            } else {
                ESP_LOGW(TAG, "Unknown effect '%s'", change->effect);
            }
        }
        if (fields & LIGHT_FIELD_EFFECT_SPEED) {
            light->effect_speed = light_clamp_range(change->effect_speed, 0, 100);
        }
        if (fields & LIGHT_FIELD_EFFECT_INTENSITY) {
            light->effect_intensity = light_clamp_range(change->effect_intensity, 0, 100);
        }
        if (fields & LIGHT_FIELD_HUE2) {
            light->hue2 = change->hue2;
        }
        if (fields & LIGHT_FIELD_SATURATION2) {
            light->sat2 = change->saturation2;
        }
        if (fields & LIGHT_FIELD_BRIGHTNESS2) {
            light->val2 = light_clamp_range(change->brightness2, 0, 100);
        }
    }
#endif

    light_write_channels(light);
#if LIGHT_EFFECTS_BUILD
    if (light->is_addressable) {
        light_effects_notify();
    }
#endif
#if CONFIG_LIGHT_PERSIST_STATE
    state_dirty = true;
#endif
    return ESP_OK;
}

esp_err_t light_set_on(size_t index, bool on) {
    light_state_change_t change = {.fields = LIGHT_FIELD_ON, .on = on};
    return light_apply_change(index, &change);
}

esp_err_t light_toggle(size_t index) {
    light_config_t *light = light_by_index(index);
    if (!light) {
        return ESP_ERR_NOT_FOUND;
    }
    return light_set_on(index, !light->on);
}



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

void light_save_state(void) {
    if (!state_dirty) {
        return;
    }

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

#else

void light_save_state(void) {}

#endif


esp_err_t light_init(void) {

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
    }

#if LIGHT_EFFECTS_BUILD
    // Must run after the loop above, since it needs every addressable_handle already created.
    light_effects_task_start();
#endif

    light_initialized = true;
    return ESP_OK;
}


esp_err_t light_shutdown(void) {
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
    return ESP_OK;
}
