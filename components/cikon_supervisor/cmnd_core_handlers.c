#include "esp_log.h"

#include "cmnd.h"
#include "cmnd_core_handlers.h"
#include "config_manager.h"
// #include "debug.h"
#include "json_parser.h"
#include "platform_services.h"

#define TAG "cmnd-core-handlers"

static void restart_handler(const char *args_json_str) {
    (void)args_json_str;

    esp_safe_restart();
}

static void help_handler(const char *args_json_str) {
    (void)args_json_str;

    size_t total = 0;
    const command_t *reg = cmnd_get_registry(&total);

    for (size_t i = 0; i < total; i++) {
        ESP_LOGI(TAG, "  %-15s - %s", reg[i].command_id, reg[i].description);
    }
    ESP_LOGI(TAG, "=======================================");
}

// static void log_handler(const char *args_json_str) {
//     (void)args_json_str;

//     debug_print_sys_info();
//     debug_print_config_summary();
// }

static void set_conf_handler(const char *args_json_str) {
    cJSON *json_args = json_str_as_object(args_json_str);
    if (!json_args) {
        ESP_LOGW(TAG, "Command aborted: invalid JSON arguments: %s", args_json_str);
        return;
    }

    config_manager_set_from_json(json_args);
    cJSON_Delete(json_args);
}

static void reset_conf_handler(const char *args_json_str) {
    (void)args_json_str;

    reset_nvs_partition();
    esp_safe_restart();
}

static void onboard_led_handler(const char *args_json_str) {
    logic_state_t state = json_str_as_logic_state(args_json_str);
    bool new_state;

    if (state == STATE_TOGGLE) {
        new_state = !get_onboard_led_state();
    } else {
        new_state = (state == STATE_ON) ? true : false;
    }

    ESP_LOGI(TAG, "Setting LED to %s", new_state ? "ON" : "OFF");
    onboard_led_set_state(new_state);
}

void cmnd_core_handlers_register(void) {
    // Basic system commands
    cmnd_register("restart", "Restart the device", restart_handler);
    cmnd_register("help", "Show available commands", help_handler);
    // cmnd_register("log", "Print debug information", log_handler);

    // Configuration commands
    cmnd_register("setconf", "Set configuration from JSON", set_conf_handler);
    cmnd_register("resetconf", "Reset configuration and restart", reset_conf_handler);

    // Hardware commands
    cmnd_register("onboard_led", "Set onboard LED state (on/off/toggle)", onboard_led_handler);
}
