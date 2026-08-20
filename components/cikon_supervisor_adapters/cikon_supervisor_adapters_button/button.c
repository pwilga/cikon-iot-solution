#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "button_gpio.h"
#include "esp_log.h"
#include "iot_button.h"
#include "soc/gpio_num.h"

#include "button_adapter.h"
#include "json_parser.h"
#include "supervisor.h"

#define TAG "cikon:adapter:button"

static bool initialized = false;
static button_handle_t button_handles[CONFIG_BUTTON_MAX_COUNT] = {0};
static char button_names[CONFIG_BUTTON_MAX_COUNT][16] = {0};
static uint8_t button_count = 0;
static button_event_callback_t user_callback = NULL;

void button_adapter_register_callback(button_event_callback_t callback) {
    user_callback = callback;
    ESP_LOGI(TAG, "Custom button callback %s", callback ? "registered" : "cleared");
}

const char *button_adapter_get_name(uint8_t idx) {
    if (idx >= button_count) {
        return NULL;
    }
    return button_names[idx];
}

void button_adapter_log_event(uint8_t button_idx, button_event_t event) {
    const char *name = button_adapter_get_name(button_idx);
    switch (event) {
    case BUTTON_SINGLE_CLICK:
        ESP_LOGI(TAG, "%s: Single click", name);
        break;
    case BUTTON_DOUBLE_CLICK:
        ESP_LOGI(TAG, "%s: Double click", name);
        break;
    case BUTTON_LONG_PRESS_START:
        ESP_LOGI(TAG, "%s: Long press start", name);
        break;
    default:
        ESP_LOGI(TAG, "%s: Unhandled event %d", name, event);
        break;
    }
}

static void button_event_handler(void *handle, void *usr_data) {

    if (!user_callback)
        return;

    uint8_t idx = (uint8_t)(uintptr_t)usr_data;
    button_event_t event = iot_button_get_event(handle);

    user_callback(idx, button_adapter_get_name(idx), event);
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
            ESP_LOGW(TAG, "Invalid format (expected gpio:active_level[:name]): %s", token);
            token = strtok(NULL, ",");
            continue;
        }

        *colon = '\0';
        int gpio = atoi(token);

        char *remainder = colon + 1;
        char *name_colon = strchr(remainder, ':');
        const char *name = NULL;
        if (name_colon) {
            *name_colon = '\0';
            name = name_colon + 1;
        }
        int active_level = atoi(remainder);

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

            if (name && strlen(name) > 0) {
                char *sanitized_name = sanitize(name);
                strncpy(button_names[button_count], sanitized_name,
                        sizeof(button_names[button_count]) - 1);
                button_names[button_count][sizeof(button_names[button_count]) - 1] = '\0';
                free(sanitized_name);

                ESP_LOGI(TAG, "Button %d '%s' initialized on GPIO %d (active %s)", button_count,
                         button_names[button_count], gpio, active_level ? "HIGH" : "LOW");
            } else {
                snprintf(button_names[button_count], sizeof(button_names[button_count]), "gpio%d",
                         gpio);
                ESP_LOGI(TAG, "Button %d initialized on GPIO %d (active %s, auto-named '%s')",
                         button_count, gpio, active_level ? "HIGH" : "LOW",
                         button_names[button_count]);
            }
            button_count++;
        } else {
            ESP_LOGE(TAG, "Failed to initialize button on GPIO %d", gpio);
        }

        token = strtok(NULL, ",");
    }

    if (token != NULL) {
        ESP_LOGE(TAG, "Too many buttons configured, max is %d, remaining entries were ignored",
                 CONFIG_BUTTON_MAX_COUNT);
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
        button_names[i][0] = '\0';
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