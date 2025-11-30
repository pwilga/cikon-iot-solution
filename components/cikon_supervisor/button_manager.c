#include "button_manager.h"
#include "button_gpio.h"
#include "esp_log.h"
#include "iot_button.h"
#include <stdint.h>

#include "cmnd.h"

static const char *TAG = "cikon-button-manager";
static button_handle_t button_handles[MAX_BUTTONS] = {0};
static uint8_t button_count = 0;

static void button_event_handler(void *button_handle, void *usr_data) {
    uint8_t idx = (uint8_t)(uintptr_t)usr_data;
    button_event_t event = iot_button_get_event(button_handle);

    switch (event) {
    case BUTTON_SINGLE_CLICK:
        cmnd_submit("onboard_led", "\"toggle\"");

        break;
    case BUTTON_DOUBLE_CLICK:
        cmnd_submit("sta", NULL);

        break;
    // case BUTTON_MULTIPLE_CLICK:
    //     ESP_LOGI(TAG, "Button %d: MULTI CLICK (>=3)", idx);
    //     break;
    case BUTTON_LONG_PRESS_START:

        cmnd_submit("ap", NULL);

        break;
    // case BUTTON_PRESS_DOWN:
    //     ESP_LOGI(TAG, "Button %d: PRESS DOWN", idx);
    //     break;
    // case BUTTON_PRESS_UP:
    //     ESP_LOGI(TAG, "Button %d: PRESS UP", idx);
    //     break;
    default:
        ESP_LOGI(TAG, "Button %d: event %d", idx, event);
        break;
    }
}

void button_manager_init(uint8_t gpio_num) {

    button_config_t btn_cfg = {
        .short_press_time = 180,
        .long_press_time = 1500,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num = gpio_num,
        .active_level = 0, // 0 = active low state
        .enable_power_save = false,
        .disable_pull = false,
    };
    button_handle_t btn = NULL;
    if (iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn) == ESP_OK &&
        button_count < MAX_BUTTONS) {
        button_handles[button_count] = btn;
        // iot_button_register_cb(btn, BUTTON_PRESS_DOWN, NULL, button_event_handler,
        //                        (void *)(uintptr_t)button_count);
        // iot_button_register_cb(btn, BUTTON_PRESS_UP, NULL, button_event_handler,
        //                        (void *)(uintptr_t)button_count);
        iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, button_event_handler,
                               (void *)(uintptr_t)button_count);
        iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK, NULL, button_event_handler,
                               (void *)(uintptr_t)button_count);
        // iot_button_register_cb(btn, BUTTON_MULTIPLE_CLICK, NULL, button_event_handler,
        //                        (void *)(uintptr_t)button_count);
        iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, button_event_handler,
                               (void *)(uintptr_t)button_count);
        button_count++;
    }
}

void button_manager_init_multi(const uint8_t *gpio_list, uint8_t num_buttons) {
    for (uint8_t i = 0; i < num_buttons && i < MAX_BUTTONS; ++i) {
        button_manager_init(gpio_list[i]);
    }
}
