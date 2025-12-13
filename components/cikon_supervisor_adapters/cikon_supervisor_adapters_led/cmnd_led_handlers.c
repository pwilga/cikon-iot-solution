#include "cJSON.h"
#include "esp_log.h"

#include "cmnd.h"
#include "cmnd_led_handlers.h"
#include "json_parser.h"
#include "led_adapter.h"

#define TAG "cikon-cmnd-led-handlers"

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

static const command_entry_t led_commands[] = {
    {"pwm_led", "Set LED brightness (0-255)", led_handler}, {NULL, NULL, NULL}};

void led_cmnd_handlers_register(void) { cmnd_register_group(led_commands); }

void led_cmnd_handlers_unregister(void) { cmnd_unregister_group(led_commands); }
