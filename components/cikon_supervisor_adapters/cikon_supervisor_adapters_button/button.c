// TODO: Test on many buttons, currently tested just for one.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "button_gpio.h"
#include "esp_log.h"
#include "iot_button.h"
#include "soc/gpio_num.h"

#include "button_adapter.h"
#include "supervisor.h"

#define TAG "cikon:adapter:button"

static bool initialized = false;
static button_handle_t button_handles[CONFIG_BUTTON_MAX_COUNT] = {0};
static uint8_t button_count = 0;
static button_event_callback_t user_callback = NULL;

void button_adapter_register_callback(button_event_callback_t callback) {
    user_callback = callback;
    ESP_LOGI(TAG, "Custom button callback %s", callback ? "registered" : "cleared");
}

static void button_event_handler(void *handle, void *usr_data) {

    if (!user_callback)
        return;

    uint8_t idx = (uint8_t)(uintptr_t)usr_data;
    button_event_t event = iot_button_get_event(handle);

    user_callback(idx, event);
}

static esp_err_t button_adapter_init(void) {

    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing button adapter");

    // Parse GPIO list from config
    char *str = strdup(CONFIG_BUTTON_GPIO_LIST);
    char *token = strtok(str, ",");

    while (token != NULL && button_count < CONFIG_BUTTON_MAX_COUNT) {
        // Trim whitespace
        while (*token == ' ')
            token++;

        char *colon = strchr(token, ':');
        if (!colon) {
            ESP_LOGW(TAG, "Invalid format (expected gpio:active_level): %s", token);
            token = strtok(NULL, ",");
            continue;
        }

        *colon = '\0';
        int gpio = atoi(token);
        int active_level = atoi(colon + 1);

        if (gpio < 0 || gpio >= SOC_GPIO_PIN_COUNT) {
            ESP_LOGW(TAG, "Invalid GPIO %d, skipping", gpio);
            token = strtok(NULL, ",");
            continue;
        }

        if (active_level != 0 && active_level != 1) {
            ESP_LOGW(TAG, "Invalid active_level %d for GPIO %d (must be 0 or 1), skipping",
                     active_level, gpio);
            token = strtok(NULL, ",");
            continue;
        }

        // Configure button
        button_config_t btn_cfg = {
            .short_press_time = CONFIG_BUTTON_SHORT_PRESS_TIME_MS,
            .long_press_time = CONFIG_BUTTON_LONG_PRESS_TIME_MS,
        };

        button_gpio_config_t gpio_cfg = {
            .gpio_num = (gpio_num_t)gpio,
            .active_level = (uint8_t)active_level,
            .enable_power_save = false,
            .disable_pull = false,
        };

        button_handle_t btn = NULL;
        if (iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn) == ESP_OK) {
            button_handles[button_count] = btn;

            iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, button_event_handler,
                                   (void *)(uintptr_t)button_count);
            iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK, NULL, button_event_handler,
                                   (void *)(uintptr_t)button_count);
            iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, button_event_handler,
                                   (void *)(uintptr_t)button_count);

            ESP_LOGI(TAG, "Button %d initialized on GPIO %d (active %s)", button_count, gpio,
                     active_level ? "HIGH" : "LOW");
            button_count++;
        } else {
            ESP_LOGE(TAG, "Failed to initialize button on GPIO %d", gpio);
        }

        token = strtok(NULL, ",");
    }

    free(str);

    if (button_count == 0) {
        ESP_LOGE(TAG, "No buttons initialized");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Button adapter initialized with %d button(s)", button_count);
    initialized = true;
    return ESP_OK;
}

static esp_err_t button_adapter_shutdown(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Shutting down button adapter");

    for (int i = 0; i < CONFIG_BUTTON_MAX_COUNT; i++) {
        if (button_handles[i]) {
            iot_button_delete(button_handles[i]);
            button_handles[i] = NULL;
        }
    }
    button_count = 0;
    initialized = false;
    return ESP_OK;
}

supervisor_platform_adapter_t button_adapter = {
    .name = "button",
    .init = button_adapter_init,
    .shutdown = button_adapter_shutdown,
};