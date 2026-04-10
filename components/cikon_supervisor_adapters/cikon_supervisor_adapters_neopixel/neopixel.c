#include "bits_helper.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "cmnd.h"
#include "metadata.h"
#include "neopixel.h"
#include "neopixel_adapter.h"
#include "neopixel_colors.h"
#include "neopixel_effects.h"
#include "supervisor.h"
#include "tele.h"

#define TAG "cikon:adapter:neopixel"

static bool initialized = false;

static const char *effect_to_str(neopixel_effect_t effect) {
    switch (effect) {
    case NEOPIXEL_EFFECT_SOLID:
        return "solid";
    case NEOPIXEL_EFFECT_BLINK:
        return "blink";
    case NEOPIXEL_EFFECT_PULSE:
        return "pulse";
    case NEOPIXEL_EFFECT_RAINBOW:
        return "rainbow";
    default:
        return "none";
    }
}

static neopixel_effect_t str_to_effect(const char *str) {
    if (!str)
        return NEOPIXEL_EFFECT_NONE;
    if (strcmp(str, "solid") == 0)
        return NEOPIXEL_EFFECT_SOLID;
    if (strcmp(str, "blink") == 0)
        return NEOPIXEL_EFFECT_BLINK;
    if (strcmp(str, "pulse") == 0)
        return NEOPIXEL_EFFECT_PULSE;
    if (strcmp(str, "rainbow") == 0)
        return NEOPIXEL_EFFECT_RAINBOW;
    return NEOPIXEL_EFFECT_NONE;
}

// Parse "#RRGGBB" or "RRGGBB" hex string → 0x00RRGGBB
static bool parse_color(const char *hex, uint32_t *out) {
    if (!hex)
        return false;
    if (hex[0] == '#')
        hex++;
    if (strlen(hex) != 6)
        return false;
    char *end;
    *out = (uint32_t)strtoul(hex, &end, 16);
    return end == hex + 6;
}

static void cmnd_neopixel(const char *payload) {
    if (!payload)
        return;

    cJSON *json = cJSON_Parse(payload);
    if (!json) {
        ESP_LOGE(TAG, "Invalid JSON: %s", payload);
        return;
    }

    // {"color":"#RRGGBB"}
    cJSON *color_item = cJSON_GetObjectItem(json, "color");
    if (color_item && cJSON_IsString(color_item)) {
        uint32_t color;
        if (parse_color(color_item->valuestring, &color)) {
            neopixel_effect_stop();
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;
            neopixel_fill(r, g, b);
            neopixel_show();
            ESP_LOGI(TAG, "Color set to #%06" PRIX32, color);
        } else {
            ESP_LOGW(TAG, "Invalid color: %s", color_item->valuestring);
        }
    }

    // {"effect":"rainbow","speed":5}
    cJSON *effect_item = cJSON_GetObjectItem(json, "effect");
    if (effect_item && cJSON_IsString(effect_item)) {
        neopixel_effect_t effect = str_to_effect(effect_item->valuestring);

        if (effect == NEOPIXEL_EFFECT_NONE) {
            neopixel_effect_stop();
        } else {
            cJSON *speed_item = cJSON_GetObjectItem(json, "speed");
            uint8_t speed = speed_item ? (uint8_t)speed_item->valueint : 5;

            cJSON *color_for_effect = cJSON_GetObjectItem(json, "color");
            uint32_t color = 0xFF0000; // default red
            if (color_for_effect && cJSON_IsString(color_for_effect)) {
                parse_color(color_for_effect->valuestring, &color);
            }

            neopixel_effect_start(effect, color, speed);
            ESP_LOGI(TAG, "Effect: %s speed=%d", effect_item->valuestring, speed);
        }
    }

    // {"brightness":128}
    cJSON *brightness_item = cJSON_GetObjectItem(json, "brightness");
    if (brightness_item && cJSON_IsNumber(brightness_item)) {
        uint8_t br = (uint8_t)brightness_item->valueint;
        neopixel_set_brightness(br);
        ESP_LOGI(TAG, "Brightness: %d", br);
    }

    cJSON_Delete(json);
}

static void tele_neopixel_state(const char *tele_id, cJSON *json_root) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "effect", effect_to_str(neopixel_effect_get_current()));
    cJSON_AddNumberToObject(obj, "brightness", neopixel_get_brightness());
    cJSON_AddItemToObject(json_root, tele_id, obj);
}

static esp_err_t neopixel_adapter_init(void) {
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing neopixel adapter");

    esp_err_t err = neopixel_init(CONFIG_NEOPIXEL_GPIO_PIN, CONFIG_NEOPIXEL_LED_COUNT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "neopixel_init failed: %s", esp_err_to_name(err));
        return err;
    }

    neopixel_set_brightness(CONFIG_NEOPIXEL_DEFAULT_BRIGHTNESS);

    neopixel_effect_start(NEOPIXEL_EFFECT_SOLID, NEOPIXEL_COLOR_RED, 0);

    initialized = true;
    return ESP_OK;
}

static esp_err_t neopixel_adapter_shutdown(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Shutting down neopixel adapter");

    neopixel_effect_stop();
    neopixel_deinit();

    initialized = false;
    return ESP_OK;
}

static void neopixel_adapter_on_event(EventBits_t bits) {
    if (bits & INET_EVENT_STA_READY) {
        neopixel_effect_start(NEOPIXEL_EFFECT_SOLID, NEOPIXEL_COLOR_GREEN, 0);
    }
}

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
static const ha_metadata_t neopixel_ha_metadata = {
    .magic = HA_METADATA_MAGIC,
    .entities = {{.type = HA_LIGHT, .name = "NeoPixel"}, {.type = HA_ENTITY_NONE}}};
#endif

supervisor_platform_adapter_t neopixel_adapter = {
    .name = "neopixel",
    .init = neopixel_adapter_init,
    .shutdown = neopixel_adapter_shutdown,
    .on_event = neopixel_adapter_on_event,
    .tele_group = (const tele_entry_t[]){{"neopixel", tele_neopixel_state}, {NULL, NULL}},
    .cmnd_group =
        (const command_entry_t[]){
            {"neopixel", "Control NeoPixel (color/effect/brightness)", cmnd_neopixel},
            {NULL, NULL, NULL}},
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    .metadata = &neopixel_ha_metadata,
#endif
};
