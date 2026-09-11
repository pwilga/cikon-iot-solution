// Parsing of CONFIG_LIGHT_GPIO_LIST into the lights[] table. One entry point,
// light_config_parse(), called once at init; nothing here runs again afterwards.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "soc/gpio_num.h"
#include "soc/soc_caps.h"

#include "json_parser.h" // sanitize()
#include "light_internal.h"

#define TAG "cikon:adapter:light"

#ifdef LIGHT_HAS_PWM
// Next free LEDC channel, handed out as lights are parsed - only light_config_parse() reads or
// advances it, since assigning channels is part of parsing a light.
static uint8_t next_ledc_channel = 0;
#endif

// Maps a channel role bitmask to its capabilities. Only combinations expressible as "one
// token per role" are supported - RGBCC/RGBWW (two channels of the same white) would need a
// duplicate-role token, which the config syntax doesn't have.
static bool light_config_shape_from_mask(uint8_t mask, bool *has_color, bool *has_white,
                                         bool *has_cct) {
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
        // has_white here is a placeholder - light_config_parse() overrides it per-light from
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
// strtok - this is called from inside light_config_parse()'s own strtok(str, ",") loop, and a
// nested strtok() would clobber the outer one's state).
static bool light_config_parse_channels(char *channels_str, light_channel_t *out_channels,
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
#ifdef LIGHT_HAS_PWM
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
#endif
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

    // Running out of channel slots with input left is rejected rather than truncated: the
    // leftover tokens would be dropped silently, and the roles that did fit can still form a
    // shape light_config_shape_from_mask() accepts. "R+G+B+C+W+S" would come out as a working
    // RGBCW light whose relay is never configured or driven, with nothing said about it.
    if (channel_count == LIGHT_MAX_CHANNELS && cursor && *cursor) {
        ESP_LOGW(TAG, "More than %d channels in a light, starting at '%s'", LIGHT_MAX_CHANNELS,
                 cursor);
        return false;
    }

    *out_channel_count = channel_count;
    return channel_count > 0;
}

void light_config_parse(void) {

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

        if (!light_config_parse_channels(token, parsed, &parsed_count)) {
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

        if (!light_config_shape_from_mask(role_mask, &light->has_color, &light->has_white,
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

#ifdef LIGHT_HAS_PWM
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
#endif

        memcpy(light->channels, parsed, sizeof(parsed));
        light->channel_count = parsed_count;
#ifdef LIGHT_HAS_PWM
        for (uint8_t c = 0; c < parsed_count; c++) {
            light->channels[c].ledc_ch =
                skip_ledc_alloc ? (ledc_channel_t)0 : (ledc_channel_t)next_ledc_channel++;
        }
#endif

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
