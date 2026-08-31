#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "soc/gpio_num.h"
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
#define LIGHT_GAMMA 2.2f
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
static uint8_t next_ledc_channel = 0;
static uint8_t gamma_lut[101];

static void light_gamma_init(void) {
    for (int i = 0; i <= 100; i++) {
        uint8_t duty = (uint8_t)roundf(powf(i / 100.0f, LIGHT_GAMMA) * 255.0f);
        // Gamma curve rounds anything below ~6% to a duty of 0 (fully off), even
        // though the user asked for a nonzero brightness - keep it just visible.
        gamma_lut[i] = (i > 0 && duty == 0) ? 1 : duty;
    }
}

void light_hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value, uint8_t *red, uint8_t *green,
                      uint8_t *blue) {

    float saturation_frac = saturation / 100.0f;
    float value_frac = value / 100.0f;
    float chroma = value_frac * saturation_frac;
    float second_component = chroma * (1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f));
    float match = value_frac - chroma;
    float r_prime, g_prime, b_prime;

    if (hue < 60) {
        r_prime = chroma, g_prime = second_component, b_prime = 0;
    } else if (hue < 120) {
        r_prime = second_component, g_prime = chroma, b_prime = 0;
    } else if (hue < 180) {
        r_prime = 0, g_prime = chroma, b_prime = second_component;
    } else if (hue < 240) {
        r_prime = 0, g_prime = second_component, b_prime = chroma;
    } else if (hue < 300) {
        r_prime = second_component, g_prime = 0, b_prime = chroma;
    } else {
        r_prime = chroma, g_prime = 0, b_prime = second_component;
    }

    *red = (uint8_t)roundf((r_prime + match) * 255.0f);
    *green = (uint8_t)roundf((g_prime + match) * 255.0f);
    *blue = (uint8_t)roundf((b_prime + match) * 255.0f);
}

// Maps a channel role bitmask to its capabilities. Only combinations expressible as "one
// token per role" are supported - RGBCC/RGBWW (two channels of the same white) would need a
// duplicate-role token, which the config syntax doesn't have.
static bool light_shape_from_mask(uint8_t mask, bool *has_color, bool *has_white, bool *has_cct) {
    static const struct {
        uint8_t mask;
        bool has_color, has_white, has_cct;
    } table[] = {
        {(1u << CH_COLD_WHITE), false, true, false},
        {(1u << CH_WARM_WHITE), false, true, false},
        {(1u << CH_COLD_WHITE) | (1u << CH_WARM_WHITE), false, true, true},
        {(1u << CH_RED) | (1u << CH_GREEN) | (1u << CH_BLUE), true, false, false},
        {(1u << CH_RED) | (1u << CH_GREEN) | (1u << CH_BLUE) | (1u << CH_COLD_WHITE), true, true,
         false},
        {(1u << CH_RED) | (1u << CH_GREEN) | (1u << CH_BLUE) | (1u << CH_WARM_WHITE), true, true,
         false},
        {(1u << CH_RED) | (1u << CH_GREEN) | (1u << CH_BLUE) | (1u << CH_COLD_WHITE) |
             (1u << CH_WARM_WHITE),
         true, true, true},
        {(1u << CH_SWITCH), false, false, false},
#ifdef LIGHT_HAS_ADDRESSABLE
        // has_white here is a placeholder - light_parse_list() overrides it per-light from
        // the channel's actual format suffix (grb vs grbw/rgbw), since that's a per-token
        // attribute the role bitmask alone can't express.
        {(1u << CH_ADDRESSABLE), true, false, false},
#endif
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].mask == mask) {
            *has_color = table[i].has_color;
            *has_white = table[i].has_white;
            *has_cct = table[i].has_cct;
            return true;
        }
    }
    return false;
}

// Splits the '+'-joined channel tokens of a single light by hand-scanning for '+' (not
// strtok - this is called from inside light_parse_list()'s own strtok(str, ",") loop, and a
// nested strtok() would clobber the outer one's state).
static bool light_parse_channels(char *channels_str, light_channel_t *out_channels,
                                 uint8_t *out_channel_count) {
    uint8_t channel_count = 0;
    uint8_t role_mask = 0;
    char *cursor = channels_str;

    while (cursor && *cursor && channel_count < LIGHT_MAX_CHANNELS) {
        char *separator = strchr(cursor, '+');
        if (separator) {
            *separator = '\0';
        }

        light_channel_role_t role;
        switch (toupper((unsigned char)cursor[0])) {
        case 'R':
            role = CH_RED;
            break;
        case 'G':
            role = CH_GREEN;
            break;
        case 'B':
            role = CH_BLUE;
            break;
        case 'C':
            role = CH_COLD_WHITE;
            break;
        case 'W':
            role = CH_WARM_WHITE;
            break;
        case 'S':
            role = CH_SWITCH;
            break;
#ifdef LIGHT_HAS_ADDRESSABLE
        case 'N':
            role = CH_ADDRESSABLE;
            break;
#endif
        default:
            ESP_LOGW(TAG, "Unknown channel role '%c' in '%s'", cursor[0], channels_str);
            return false;
        }

        if (role_mask & (1u << role)) {
            ESP_LOGW(TAG, "Duplicate channel role in '%s'", channels_str);
            return false;
        }

        char *digits_end = cursor + 1;
        while (isdigit((unsigned char)*digits_end)) {
            digits_end++;
        }

        int gpio = atoi(cursor + 1);
        if (gpio < 0 || gpio >= SOC_GPIO_PIN_COUNT) {
            ESP_LOGW(TAG, "Invalid GPIO %d in '%s'", gpio, channels_str);
            return false;
        }

        bool active_level = true; // default active-high; only meaningful for role CH_SWITCH
        if (role == CH_SWITCH) {
            char suffix = (char)toupper((unsigned char)*digits_end);
            if (suffix == 'L') {
                active_level = false;
            } else if (suffix != '\0' && suffix != 'H') {
                ESP_LOGW(TAG,
                         "Invalid active-level suffix '%c' for S channel in '%s' (expected "
                         "'H' or 'L')",
                         *digits_end, channels_str);
                return false;
            }
        }

#ifdef LIGHT_HAS_ADDRESSABLE
        uint16_t led_count = 0;
        led_color_component_format_t led_color_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
        bool has_white_channel = false;
        if (role == CH_ADDRESSABLE) {
            if (toupper((unsigned char)*digits_end) != 'X') {
                ESP_LOGW(TAG, "Addressable channel '%s' missing 'x<led count>' (e.g. N4x30)",
                         channels_str);
                return false;
            }
            char *count_str = digits_end + 1;
            char *count_end = count_str;
            while (isdigit((unsigned char)*count_end)) {
                count_end++;
            }
            if (count_end == count_str) {
                ESP_LOGW(TAG, "Addressable channel '%s' has invalid LED count", channels_str);
                return false;
            }
            led_count = (uint16_t)atoi(count_str);
            if (led_count == 0) {
                ESP_LOGW(TAG, "Addressable channel '%s' has zero LED count", channels_str);
                return false;
            }

            // Trailing letters after the count = led_strip color component format name
            // (grb/rgb/grbw/rgbw) - different WS2812 batches/clones use different channel
            // order, and some strips (e.g. SK6812) have a 4th, separate white channel.
            char format_buf[8] = {0};
            size_t format_len = 0;
            for (char *p = count_end; *p && format_len < sizeof(format_buf) - 1; p++) {
                format_buf[format_len++] = (char)tolower((unsigned char)*p);
            }

            if (format_len == 0 || strcmp(format_buf, "grb") == 0) {
                led_color_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
            } else if (strcmp(format_buf, "rgb") == 0) {
                led_color_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB;
            } else if (strcmp(format_buf, "grbw") == 0) {
                led_color_format = LED_STRIP_COLOR_COMPONENT_FMT_GRBW;
                has_white_channel = true;
            } else if (strcmp(format_buf, "rgbw") == 0) {
                led_color_format = LED_STRIP_COLOR_COMPONENT_FMT_RGBW;
                has_white_channel = true;
            } else {
                ESP_LOGW(TAG, "Addressable channel '%s' has unknown color format '%s'",
                         channels_str, format_buf);
                return false;
            }
        }
#endif

        role_mask |= (1u << role);
        out_channels[channel_count].gpio = (gpio_num_t)gpio;
        out_channels[channel_count].role = role;
        out_channels[channel_count].active_level = active_level;
#ifdef LIGHT_HAS_ADDRESSABLE
        out_channels[channel_count].led_count = led_count;
        out_channels[channel_count].led_color_format = led_color_format;
        out_channels[channel_count].has_white_channel = has_white_channel;
#endif
        channel_count++;

        cursor = separator ? separator + 1 : NULL;
    }

    *out_channel_count = channel_count;
    return channel_count > 0;
}

static void light_parse_list(void) {

    const char *list_str = CONFIG_LIGHT_GPIO_LIST;
    char *str = strdup(list_str);
    char *token = strtok(str, ",");
    uint8_t index = 0;

    for (int i = 0; i <= CONFIG_LIGHT_MAX_COUNT; i++) {
        lights[i].channel_count = 0;
    }

    while (token != NULL && index < CONFIG_LIGHT_MAX_COUNT) {
        while (*token == ' ') {
            token++;
        }

        char *colon = strchr(token, ':');
        const char *name = NULL;
        if (colon) {
            *colon = '\0';
            name = colon + 1;
        }

        light_config_t *light = &lights[index];
        light_channel_t parsed[LIGHT_MAX_CHANNELS];
        uint8_t parsed_count = 0;
        uint8_t role_mask = 0;

        if (!light_parse_channels(token, parsed, &parsed_count)) {
            ESP_LOGE(TAG, "Skipping light with invalid channel list '%s'", token);
            token = strtok(NULL, ",");
            continue;
        }

        for (uint8_t c = 0; c < parsed_count; c++) {
            role_mask |= (1u << parsed[c].role);
        }

        bool is_switch = (role_mask == (1u << CH_SWITCH));
#ifdef LIGHT_HAS_ADDRESSABLE
        bool is_addressable = (role_mask == (1u << CH_ADDRESSABLE));
#endif

        if (!light_shape_from_mask(role_mask, &light->has_color, &light->has_white,
                                   &light->has_cct)) {
            ESP_LOGE(TAG, "Skipping light '%s': unsupported channel combination", token);
            token = strtok(NULL, ",");
            continue;
        }
        light->is_switch = is_switch;
#ifdef LIGHT_HAS_ADDRESSABLE
        light->is_addressable = is_addressable;
        // has_white can't be expressed by role_mask alone for CH_ADDRESSABLE (single role bit
        // covers both grb and grbw tokens) - override from the actual parsed format suffix.
        if (is_addressable) {
            light->has_white = parsed[0].has_white_channel;
        }
#endif

        // default values
        light->val = 50;
        light->sat = 100;
        light->hue = 0;
        light->cct = 50;
        light->color_mode = light->has_color && !light->has_white;
#if LIGHT_EFFECTS_BUILD
        // WLED's own DEFAULT_SPEED/DEFAULT_INTENSITY are 128/255 (~50%) - same proportion here.
        light->effect_speed = 50;
        light->effect_intensity = 50;
        // val2=0 (not just sat2=0) is required for a true default-black second color - see
        // light_internal.h.
        light->hue2 = 0;
        light->sat2 = 0;
        light->val2 = 0;
#endif

#ifdef LIGHT_HAS_ADDRESSABLE
        bool skip_ledc_alloc = is_switch || is_addressable;
#else
        bool skip_ledc_alloc = is_switch;
#endif
        if (!skip_ledc_alloc && next_ledc_channel + parsed_count > SOC_LEDC_CHANNEL_NUM) {
            ESP_LOGE(TAG, "Skipping light '%s': not enough LEDC channels left", token);
            token = strtok(NULL, ",");
            continue;
        }

        memcpy(light->channels, parsed, sizeof(parsed));
        light->channel_count = parsed_count;
        for (uint8_t c = 0; c < parsed_count; c++) {
            light->channels[c].ledc_ch =
                skip_ledc_alloc ? (ledc_channel_t)0 : (ledc_channel_t)next_ledc_channel++;
        }

        if (name && strlen(name) > 0) {
            strncpy(light->name, name, sizeof(light->name) - 1);
        } else {
            snprintf(light->name, sizeof(light->name), "light%u", index);
        }
        light->name[sizeof(light->name) - 1] = '\0';

        char *sanitized_name = sanitize(light->name);
        strncpy(light->name, sanitized_name, sizeof(light->name) - 1);
        light->name[sizeof(light->name) - 1] = '\0';
        free(sanitized_name);

        ESP_LOGI(TAG, "Configured light %d '%s' (%d channel(s), color=%d white=%d cct=%d)", index,
                 light->name, light->channel_count, light->has_color, light->has_white,
                 light->has_cct);

        index++;
        token = strtok(NULL, ",");
    }

    if (token != NULL) {
        ESP_LOGE(TAG, "Too many lights configured, max is %d, remaining entries were ignored",
                 CONFIG_LIGHT_MAX_COUNT);
    }

    free(str);
}

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
// Dispatches to the has_white-aware led_strip call for one pixel - the unit light_addressable_fill
// loops over, and shared with light_effects.c so per-pixel effects don't each duplicate this
// branch. w is ignored when has_white is false. Non-static: shared with light_effects.c.
void light_addressable_set_pixel(led_strip_handle_t handle, uint16_t i, bool has_white, uint8_t r,
                                 uint8_t g, uint8_t b, uint8_t w) {
    if (has_white) {
        led_strip_set_pixel_rgbw(handle, i, r, g, b, w);
    } else {
        led_strip_set_pixel(handle, i, r, g, b);
    }
}

// led_strip has no "fill all pixels" of its own - only per-pixel set_pixel/set_pixel_rgbw.
void light_addressable_fill(led_strip_handle_t handle, uint16_t count, bool has_white, uint8_t r,
                            uint8_t g, uint8_t b, uint8_t w) {
    for (uint16_t i = 0; i < count; i++) {
        light_addressable_set_pixel(handle, i, has_white, r, g, b, w);
    }
}
#endif

// Non-static: shared with light_effects.c, which needs the same on/color_mode/white/cct ->
// r,g,b,c,w mapping to render a solid (effect == NONE) addressable light.
void light_compute_rgbcw(light_config_t *light, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *c,
                         uint8_t *w) {
    *r = *g = *b = *c = *w = 0;

    if (!light->on) {
        return;
    }

    if (light->color_mode && light->has_color) {
        light_hsv_to_rgb(light->hue, light->sat, light->val, r, g, b);
    } else if (light->has_white) {
        if (light->has_cct) {
            *c = gamma_lut[(light->val * light->cct) / 100];
            *w = gamma_lut[(light->val * (100 - light->cct)) / 100];
            // Splitting val between two channels before the gamma lookup can
            // round both shares down to 0 at low brightness (e.g. val=1,
            // cct=50: 50/100 truncates to 0 twice), even though val > 0 -
            // keep the dominant channel of the mix at least barely lit.
            if (light->val > 0 && *c == 0 && *w == 0) {
                if (light->cct >= 50) {
                    *c = 1;
                } else {
                    *w = 1;
                }
            }
        } else {
            uint8_t value = gamma_lut[light->val];
            *c = value;
            *w = value;
        }
    }
}

static void light_drive_hardware(light_config_t *light) {
    uint8_t r, g, b, c, w;
    light_compute_rgbcw(light, &r, &g, &b, &c, &w);

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
                light_addressable_fill(light->addressable_handle, light->channels[i].led_count,
                                       light->channels[i].has_white_channel, r, g, b, w);
                led_strip_refresh(light->addressable_handle);
            }
#endif
            continue;
        }
#endif

        uint8_t value = 0;
        switch (light->channels[i].role) {
        case CH_RED:
            value = r;
            break;
        case CH_GREEN:
            value = g;
            break;
        case CH_BLUE:
            value = b;
            break;
        case CH_COLD_WHITE:
            value = c;
            break;
        case CH_WARM_WHITE:
            value = w;
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

    light_drive_hardware(light);
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

    light_gamma_init();
    light_parse_list();

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

        light_drive_hardware(light);

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
    light_drive_hardware(&lights[idx]);
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
                light_hsv_to_rgb(light->hue, light->sat, light->val, &r, &g, &b);
            } else {
                // Not RGBW/RGBCW's actual output (that's computed separately in
                // light_compute_rgbcw) - just an approximate warm<->cool tint from cct, so
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
                if (has_c && has_w) {
                    c_val = gamma_lut[(light->val * light->cct) / 100];
                    w_val = gamma_lut[(light->val * (100 - light->cct)) / 100];
                } else {
                    uint8_t value = gamma_lut[light->val];
                    c_val = value;
                    w_val = value;
                }
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
