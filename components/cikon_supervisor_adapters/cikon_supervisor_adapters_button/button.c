#include "button_adapter.h"
#include "button_gpio.h"
#include "cmnd.h"
#include "esp_log.h"
#include "iot_button.h"
#include "sdkconfig.h"
#include "supervisor.h"
#include <stdint.h>

#define TAG "cikon:adapter:button"
#define MAX_BUTTONS 4

static button_handle_t button_handles[MAX_BUTTONS] = {0};
static uint8_t button_count = 0;

static void button_event_handler(void *handle, void *usr_data) {
    uint8_t idx = (uint8_t)(uintptr_t)usr_data;
    button_event_t event = iot_button_get_event(handle);

    switch (event) {
    case BUTTON_SINGLE_CLICK:
        ESP_LOGI(TAG, "Button %d: Single click", idx);
        cmnd_submit("onboard_led", "\"toggle\"");
        break;
    case BUTTON_DOUBLE_CLICK:
        ESP_LOGI(TAG, "Button %d: Double click", idx);
        cmnd_submit("sta", NULL);
        break;
    case BUTTON_LONG_PRESS_START:
        ESP_LOGI(TAG, "Button %d: Long press", idx);
        cmnd_submit("ap", NULL);
        break;
    default:
        ESP_LOGI(TAG, "Button %d: Event %d", idx, event);
        break;
    }
}

static void button_adapter_init(void) {

    ESP_LOGI(TAG, "Initializing button adapter on GPIO %d", CONFIG_BUTTON_GPIO);

    button_config_t btn_cfg = {
        .short_press_time = CONFIG_BUTTON_SHORT_PRESS_TIME_MS,
        .long_press_time = CONFIG_BUTTON_LONG_PRESS_TIME_MS,
    };

    button_gpio_config_t gpio_cfg = {
        .gpio_num = CONFIG_BUTTON_GPIO,
        .active_level = CONFIG_BUTTON_ACTIVE_LEVEL,
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

        button_count++;
        ESP_LOGI(TAG, "Button initialized successfully (total: %d)", button_count);
    } else {
        ESP_LOGE(TAG, "Failed to initialize button");
    }
}

static void button_adapter_shutdown(void) {
    ESP_LOGI(TAG, "Shutting down button adapter");

    for (int i = 0; i < MAX_BUTTONS; i++) {
        if (button_handles[i]) {
            iot_button_delete(button_handles[i]);
            button_handles[i] = NULL;
        }
    }
    button_count = 0;
}

supervisor_platform_adapter_t button_adapter = {.init = button_adapter_init,
                                                .shutdown = button_adapter_shutdown,
                                                .on_event = NULL,
                                                .on_interval = NULL};
