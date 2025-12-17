#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "soc/gpio_num.h"

#include "cJSON.h"

#include "cmnd.h"
#include "config_manager.h"
#include "ha.h"
#include "json_parser.h"
#include "led_adapter.h"
#include "supervisor.h"
#include "tele.h"

#define TAG "cikon:adapter:led"
#define LED_ADAPTER_MAX_LEDS LEDC_CHANNEL_MAX

typedef struct {
    gpio_num_t gpio;
    ledc_channel_t channel;
    uint8_t brightness;
    uint8_t last_brightness;
    char name[16];
} led_config_t;

static led_config_t leds[LED_ADAPTER_MAX_LEDS + 1]; // +1 for sentinel
static bool led_initialized = false;
static uint64_t last_saved_state = 0;
static uint64_t last_saved_last_state = 0;
static bool state_dirty = false;

static void parse_gpio_list(void) {

    const char *gpio_list = CONFIG_LED_GPIO_LIST;
    char *str = strdup(gpio_list);
    char *token = strtok(str, ",");
    int index = 0;

    // Initialize all with sentinel first
    for (int i = 0; i <= LED_ADAPTER_MAX_LEDS; i++) {
        leds[i].gpio = GPIO_NUM_NC;
    }

    while (token != NULL && index < LED_ADAPTER_MAX_LEDS) {
        char *colon = strchr(token, ':');
        int gpio;
        const char *name = NULL;

        if (colon) {
            *colon = '\0';
            gpio = atoi(token);
            name = colon + 1;
        } else {
            gpio = atoi(token);
        }

        if (gpio >= 0 && gpio < SOC_GPIO_PIN_COUNT) {
            leds[index].gpio = (gpio_num_t)gpio;
            leds[index].channel = (ledc_channel_t)index;
            leds[index].brightness = 0;
            leds[index].last_brightness = 255;

            if (name && strlen(name) > 0) {
                strncpy(leds[index].name, name, sizeof(leds[index].name) - 1);
                leds[index].name[sizeof(leds[index].name) - 1] = '\0';
                ESP_LOGI(TAG, "Configured LED %d '%s' on GPIO %d", index, leds[index].name, gpio);
            } else {
                snprintf(leds[index].name, sizeof(leds[index].name), "led%d", index);
                ESP_LOGI(TAG, "Configured LED %d on GPIO %d (auto-named '%s')", index, gpio,
                         leds[index].name);
            }
            index++;
        }
        token = strtok(NULL, ",");
    }

    free(str);
}

// Macros to generate pack/unpack functions for LED state fields
// Each macro creates a function that packs/unpacks 8 uint8_t values into/from a uint64_t
// This allows storing LED state efficiently in NVS (one nvs_set_u64 per field)

// Generate: static uint64_t led_pack_<field_name>(void)
// Packs leds[0..7].<field_name> into bits [0..63] (8 bits per LED)
#define LED_PACK_FIELD(field_name)                                                                 \
    static uint64_t led_pack_##field_name(void) {                                                  \
        uint64_t packed = 0;                                                                       \
        for (int i = 0; i < 8 && leds[i].gpio != GPIO_NUM_NC; i++) {                               \
            packed |= ((uint64_t)leds[i].field_name) << (i * 8);                                   \
        }                                                                                          \
        return packed;                                                                             \
    }

// Generate: static void led_unpack_<field_name>(uint64_t packed)
// Unpacks bits [0..63] into leds[0..7].<field_name> (8 bits per LED)
#define LED_UNPACK_FIELD(field_name)                                                               \
    static void led_unpack_##field_name(uint64_t packed) {                                         \
        for (int i = 0; i < 8 && leds[i].gpio != GPIO_NUM_NC; i++) {                               \
            leds[i].field_name = (packed >> (i * 8)) & 0xFF;                                       \
        }                                                                                          \
    }

LED_PACK_FIELD(brightness)
LED_PACK_FIELD(last_brightness)
LED_UNPACK_FIELD(brightness)
LED_UNPACK_FIELD(last_brightness)

static void led_save_state(void) {

    uint64_t current_state = led_pack_brightness();
    uint64_t current_last_state = led_pack_last_brightness();

    if (current_state != last_saved_state || current_last_state != last_saved_last_state) {
        config_set_led_state(current_state);
        config_set_led_last_state(current_last_state);
        last_saved_state = current_state;
        last_saved_last_state = current_last_state;
        state_dirty = false;
        ESP_LOGI(TAG, "LED state saved to NVS: 0x%" PRIx64 " / 0x%" PRIx64, current_state,
                 current_last_state);
    }
}

static void led_restore_state(void) {

    uint64_t saved_state = config_get()->led_state;
    uint64_t saved_last_state = config_get()->led_last_state;

    led_unpack_brightness(saved_state);
    led_unpack_last_brightness(saved_last_state);

    last_saved_state = saved_state;
    last_saved_last_state = saved_last_state;

    ESP_LOGI(TAG, "LED state restored from NVS: 0x%" PRIx64 " / 0x%" PRIx64, saved_state,
             saved_last_state);
}

static void led_init_channel(led_config_t *led) {

    ledc_channel_config_t channel_config = {.gpio_num = led->gpio,
                                            .speed_mode = LEDC_LOW_SPEED_MODE,
                                            .channel = led->channel,
                                            .timer_sel = LEDC_TIMER_0,
                                            .duty = led->brightness, // Use restored value
                                            .hpoint = 0};
#ifdef CONFIG_LED_OUTPUT_INVERT
    channel_config.flags.output_invert = 1;
#endif

    esp_err_t ret = ledc_channel_config(&channel_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LED channel %d on GPIO %d", led->channel, led->gpio);
        return;
    }

    ESP_LOGI(TAG, "LED channel %d initialized on GPIO %d", led->channel, led->gpio);
}

static void led_ha_register_entities(void) {
    for (uint8_t i = 0;; i++) {
        const char *name = led_get_name(i);
        if (!name) {
            break;
        }

        ha_register_entity(
            &(ha_entity_config_t){.type = HA_LIGHT, .name = name, .parent_key = "pwm_led"});
    }
}

static void led_adapter_init(void) {

    ESP_LOGI(TAG, "Initializing LED adapter");

    if (led_initialized) {
        return;
    }

    parse_gpio_list();

    ledc_timer_config_t timer_config = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                        .duty_resolution = LEDC_TIMER_8_BIT,
                                        .timer_num = LEDC_TIMER_0,
                                        .freq_hz = CONFIG_LED_PWM_FREQUENCY,
                                        .clk_cfg = LEDC_AUTO_CLK};

    if (ledc_timer_config(&timer_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC timer");
        return;
    }

    ledc_fade_func_install(0);

    led_restore_state();

    for (led_config_t *led = leds; led->gpio != GPIO_NUM_NC; led++) {
        led_init_channel(led);
    }

    led_initialized = true;
    led_ha_register_entities();
    ESP_LOGI(TAG, "LED adapter initialized");
}

static void led_adapter_shutdown(void) {
    if (!led_initialized) {
        return;
    }

    led_save_state();

    for (led_config_t *led = leds; led->gpio != GPIO_NUM_NC; led++) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, led->channel, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, led->channel);
    }

    ledc_fade_func_uninstall();
    led_initialized = false;
    ESP_LOGI(TAG, "LED adapter shutdown");
}

void led_set_brightness(uint8_t led_index, uint8_t brightness) {
    if (!led_initialized || led_index >= LED_ADAPTER_MAX_LEDS ||
        leds[led_index].gpio == GPIO_NUM_NC) {
        return;
    }

    if (brightness > 0) {
        leds[led_index].last_brightness = brightness;
    }

    leds[led_index].brightness = brightness;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, leds[led_index].channel, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, leds[led_index].channel);
    state_dirty = true;
}

void led_fade_to(uint8_t led_index, uint8_t target, uint32_t duration_ms) {
    if (!led_initialized || led_index >= LED_ADAPTER_MAX_LEDS ||
        leds[led_index].gpio == GPIO_NUM_NC) {
        return;
    }

    if (target > 0) {
        leds[led_index].last_brightness = target;
    }

    leds[led_index].brightness = target;
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, leds[led_index].channel, target, duration_ms);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, leds[led_index].channel, LEDC_FADE_NO_WAIT);
    state_dirty = true;
}

uint8_t led_get_brightness(uint8_t led_index) {
    if (led_index >= LED_ADAPTER_MAX_LEDS || leds[led_index].gpio == GPIO_NUM_NC) {
        return 0;
    }
    return leds[led_index].brightness;
}

void led_turn_on(uint8_t led_index) {
    if (!led_initialized || led_index >= LED_ADAPTER_MAX_LEDS ||
        leds[led_index].gpio == GPIO_NUM_NC) {
        return;
    }

    uint8_t brightness = leds[led_index].last_brightness;
    if (brightness == 0) {
        brightness = 255;
    }
    led_set_brightness(led_index, brightness);
}

void led_turn_off(uint8_t led_index) { led_set_brightness(led_index, 0); }

bool led_is_on(uint8_t led_index) { return led_get_brightness(led_index) > 0; }

int8_t led_find_by_name(const char *name) {
    if (!name || !led_initialized) {
        return -1;
    }

    for (int i = 0; i < LED_ADAPTER_MAX_LEDS; i++) {
        if (leds[i].gpio == GPIO_NUM_NC) {
            break;
        }
        char *sanitized_stored = sanitize(leds[i].name);
        int match = strcmp(sanitized_stored, name) == 0;
        free(sanitized_stored);
        if (match) {
            return i;
        }
    }
    return -1;
}

const char *led_get_name(uint8_t led_index) {
    if (led_index >= LED_ADAPTER_MAX_LEDS || leds[led_index].gpio == GPIO_NUM_NC) {
        return NULL;
    }
    return leds[led_index].name;
}

static void led_tele_appender(const char *tele_id, cJSON *json_root) {
    cJSON *led_obj = cJSON_CreateObject();

    for (uint8_t i = 0;; i++) {
        const char *name = led_get_name(i);
        if (!name) {
            break;
        }
        uint8_t brightness = led_get_brightness(i);
        char *sanitized_name = sanitize(name);
        cJSON_AddNumberToObject(led_obj, sanitized_name, brightness);
        free(sanitized_name);
    }

    cJSON_AddItemToObject(json_root, tele_id, led_obj);
}

static void led_handler(const char *args_json_str) {
    cJSON *root = cJSON_Parse(args_json_str);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON: %s", args_json_str);
        return;
    }

    cJSON *item = root->child;
    while (item) {
        int8_t idx = led_find_by_name(item->string);
        if (idx < 0) {
            ESP_LOGW(TAG, "LED '%s' not found", item->string);
            item = item->next;
            continue;
        }

        if (cJSON_IsNumber(item)) {
            int brightness = item->valueint;
            if (brightness >= 0 && brightness <= 255) {
                ESP_LOGI(TAG, "Setting LED '%s' brightness to %d", item->string, brightness);
                led_set_brightness(idx, (uint8_t)brightness);
            } else {
                ESP_LOGW(TAG, "Invalid brightness %d for LED '%s' (must be 0-255)", brightness,
                         item->string);
            }
        } else {
            logic_state_t state = json_str_as_logic_state(cJSON_Print(item));
            if (state == STATE_TOGGLE) {
                ESP_LOGI(TAG, "Toggling LED '%s'", item->string);
                if (led_is_on(idx)) {
                    led_turn_off(idx);
                } else {
                    led_turn_on(idx);
                }
            } else if (state == STATE_ON) {
                ESP_LOGI(TAG, "Turning LED '%s' on", item->string);
                led_turn_on(idx);
            } else {
                ESP_LOGI(TAG, "Turning LED '%s' off", item->string);
                led_turn_off(idx);
            }
        }
        item = item->next;
    }

    cJSON_Delete(root);
}

static void led_adapter_on_interval(supervisor_interval_stage_t stage) {
    // Save state every 5 seconds if dirty
    if (stage == SUPERVISOR_INTERVAL_5S && state_dirty) {
        led_save_state();
    }
}

static const tele_entry_t led_tele_group[] = {{"pwm_led", led_tele_appender}, {NULL, NULL}};

static const command_entry_t led_cmnd_group[] = {
    {"pwm_led", "Set LED brightness (0-255)", led_handler}, {NULL, NULL, NULL}};

supervisor_platform_adapter_t led_adapter = {
    .init = led_adapter_init,
    .shutdown = led_adapter_shutdown,
    .on_interval = led_adapter_on_interval,
    .tele_group = led_tele_group,
    .cmnd_group = led_cmnd_group,
};
