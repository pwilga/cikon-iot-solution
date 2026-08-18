#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "soc/gpio_num.h"
#include "soc/soc_caps.h"

#include "cJSON.h"

#include "cmnd.h"
#include "json_parser.h"
#include "light_adapter.h"
#include "metadata.h"
#include "supervisor.h"
#include "tele.h"

#define TAG "cikon:adapter:light"
#define LIGHT_MAX_CHANNELS 5
#define LIGHT_GAMMA 2.2f
#define LIGHT_KELVIN_MIN 2200
#define LIGHT_KELVIN_MAX 7000

typedef enum { CH_NONE = 0, CH_R, CH_G, CH_B, CH_C, CH_W } light_channel_role_t;

typedef struct {
    gpio_num_t gpio;
    light_channel_role_t role;
    ledc_channel_t ledc_ch;
} light_channel_t;

typedef struct {
    light_channel_t channels[LIGHT_MAX_CHANNELS];
    uint8_t channel_count; // 0 == unused slot (sentinel)
    bool has_color;        // has R+G+B
    bool has_white;        // has C and/or W
    bool has_cct;          // has C and W together (real cold/warm mixing)
    char name[16];
    bool on;
    bool color_mode;  // true = RGB output active, false = C/W output active
    uint16_t hue;     // 0-360, color mode
    uint8_t sat, val; // 0-100, color mode (val also doubles as white-mode brightness)
    uint16_t cct;     // 0-100, white mode cold/warm ratio
} light_config_t;

static light_config_t lights[CONFIG_LIGHT_MAX_COUNT + 1]; // +1 sentinel
static bool light_initialized = false;
static uint8_t next_ledc_channel = 0;
static uint8_t gamma_lut[101];

static void light_gamma_init(void) {
    for (int i = 0; i <= 100; i++) {
        gamma_lut[i] = (uint8_t)roundf(powf(i / 100.0f, LIGHT_GAMMA) * 255.0f);
    }
}

static void light_hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value, uint8_t *red,
                             uint8_t *green, uint8_t *blue) {

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
        {(1u << CH_C), false, true, false},
        {(1u << CH_W), false, true, false},
        {(1u << CH_C) | (1u << CH_W), false, true, true},
        {(1u << CH_R) | (1u << CH_G) | (1u << CH_B), true, false, false},
        {(1u << CH_R) | (1u << CH_G) | (1u << CH_B) | (1u << CH_C), true, true, false},
        {(1u << CH_R) | (1u << CH_G) | (1u << CH_B) | (1u << CH_W), true, true, false},
        {(1u << CH_R) | (1u << CH_G) | (1u << CH_B) | (1u << CH_C) | (1u << CH_W), true, true,
         true},
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
            role = CH_R;
            break;
        case 'G':
            role = CH_G;
            break;
        case 'B':
            role = CH_B;
            break;
        case 'C':
            role = CH_C;
            break;
        case 'W':
            role = CH_W;
            break;
        default:
            ESP_LOGW(TAG, "Unknown channel role '%c' in '%s'", cursor[0], channels_str);
            return false;
        }

        if (role_mask & (1u << role)) {
            ESP_LOGW(TAG, "Duplicate channel role in '%s'", channels_str);
            return false;
        }

        int gpio = atoi(cursor + 1);
        if (gpio < 0 || gpio >= SOC_GPIO_PIN_COUNT) {
            ESP_LOGW(TAG, "Invalid GPIO %d in '%s'", gpio, channels_str);
            return false;
        }

        role_mask |= (1u << role);
        out_channels[channel_count].gpio = (gpio_num_t)gpio;
        out_channels[channel_count].role = role;
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

        if (!light_shape_from_mask(role_mask, &light->has_color, &light->has_white,
                                   &light->has_cct)) {
            ESP_LOGE(TAG, "Skipping light '%s': unsupported channel combination", token);
            token = strtok(NULL, ",");
            continue;
        }

        // default values
        light->val = 50;
        light->sat = 100;
        light->hue = 0;
        light->cct = 50;
        light->color_mode = light->has_color && !light->has_white;

        if (next_ledc_channel + parsed_count > SOC_LEDC_CHANNEL_NUM) {
            ESP_LOGE(TAG, "Skipping light '%s': not enough LEDC channels left", token);
            token = strtok(NULL, ",");
            continue;
        }

        memcpy(light->channels, parsed, sizeof(parsed));
        light->channel_count = parsed_count;
        for (uint8_t c = 0; c < parsed_count; c++) {
            light->channels[c].ledc_ch = (ledc_channel_t)next_ledc_channel++;
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

static void light_update_output(light_config_t *light) {
    uint8_t r = 0, g = 0, b = 0, c = 0, w = 0;

    if (light->on) {
        if (light->color_mode && light->has_color) {
            light_hsv_to_rgb(light->hue, light->sat, light->val, &r, &g, &b);
        } else if (light->has_white) {
            if (light->has_cct) {
                c = gamma_lut[(light->val * light->cct) / 100];
                w = gamma_lut[(light->val * (100 - light->cct)) / 100];
            } else {
                uint8_t value = gamma_lut[light->val];
                c = value;
                w = value;
            }
        }
    }

    for (uint8_t i = 0; i < light->channel_count; i++) {
        uint8_t value = 0;
        switch (light->channels[i].role) {
        case CH_R:
            value = r;
            break;
        case CH_G:
            value = g;
            break;
        case CH_B:
            value = b;
            break;
        case CH_C:
            value = c;
            break;
        case CH_W:
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
                light->val = (uint8_t)v->valueint;
            }
        } else if (h || s) {
            if (h) {
                light->hue = (uint16_t)h->valueint;
            }
            if (s) {
                light->sat = (uint8_t)s->valueint;
            }
            if (v) {
                light->val = (uint8_t)v->valueint;
            }
            light->color_mode = true;
        } else if (v) {
            light->val = (uint8_t)v->valueint;
        }

        if (on) {
            light->on = cJSON_IsTrue(on);
        } else if (h || s || v || (cct && light->has_white)) {
            light->on = true;
        }
    } else {
        logic_state_t state = json_str_as_logic_state(args_json_str);
        light->on = (state == STATE_TOGGLE) ? !light->on : (state == STATE_ON);
    }

    cJSON_Delete(root);

    light_update_output(light);
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

static esp_err_t light_adapter_init(void) {

    ESP_LOGI(TAG, "Initializing light adapter");

    if (light_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    light_gamma_init();
    light_parse_list();

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

        light_update_output(light);

        if ((size_t)i >= trampoline_count) {
            ESP_LOGE(TAG,
                     "No cmnd trampoline for light '%s' (index %d) - CMake/runtime light "
                     "count mismatch",
                     light->name, i);
            break;
        }
        cmnd_register(light->name, "Set light color/CCT/state ({h,s,v,cct,on} or on/off/toggle)",
                      light_cmnd_trampolines[i]);
    }

    light_initialized = true;
    ESP_LOGI(TAG, "Light adapter initialized");
    return ESP_OK;
}

static esp_err_t light_adapter_shutdown(void) {
    if (!light_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; lights[i].channel_count != 0; i++) {
        cmnd_unregister(lights[i].name);
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
    light_update_output(&lights[idx]);
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
        cJSON_AddNumberToObject(obj, "v", light->val);

        if (light->has_cct) {
            cJSON_AddNumberToObject(obj, "cct", light->cct);
        }

        if (light->has_color) {
            uint8_t r, g, b;
            if (light->color_mode) {
                light_hsv_to_rgb(light->hue, light->sat, light->val, &r, &g, &b);
            } else {
                // Not RGBW/RGBCW's actual output (that's computed separately in
                // light_update_output) - just an approximate warm<->cool tint from cct, so
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
        bool has_c = light_has_role(light, CH_C);
        bool has_w = light_has_role(light, CH_W);
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
             "\"on\":true}}",
             sanitized_name, LIGHT_KELVIN_MIN, LIGHT_KELVIN_MAX, LIGHT_KELVIN_MIN);

    cJSON_AddStringToObject(payload, "command_on_template", cmd_buf);

    snprintf(buf, sizeof(buf), "{\"%s\":{\"on\":false}}", sanitized_name);
    cJSON_AddStringToObject(payload, "command_off_template", buf);

    snprintf(buf, sizeof(buf), "{%% if value_json.%s.on %%}on{%% else %%}off{%% endif %%}",
             sanitized_name);
    cJSON_AddStringToObject(payload, "state_template", buf);

    snprintf(buf, sizeof(buf), "{{ (value_json.%s.v / 100 * 255) | round }}", sanitized_name);
    cJSON_AddStringToObject(payload, "brightness_template", buf);

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

    cJSON_DeleteItemFromObject(payload, "val_tpl");
}

#define HA_ENTITY_ENTRY(light_name)                                                                \
    {.type = HA_LIGHT,                                                                             \
     .name = light_name,                                                                           \
     .icon = "mdi:led-strip-variant",                                                              \
     .custom_builder = light_ha_build},

static const ha_metadata_t light_ha_metadata = {
    .magic = HA_METADATA_MAGIC, .entities = {HA_ENTITY_LIST{.type = HA_ENTITY_NONE}}};
#undef HA_ENTITY_ENTRY
#endif

supervisor_platform_adapter_t light_adapter = {
    .name = "light",
    .init = light_adapter_init,
    .shutdown = light_adapter_shutdown,
    .tele_group = (const tele_entry_t[]){{"light", tele_light}, {NULL, NULL}},
    .cmnd_group = NULL, // registered dynamically per light in light_adapter_init
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    .metadata = &light_ha_metadata,
#endif
};
