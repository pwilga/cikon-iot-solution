#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "cmnd.h"
#include "config_manager.h"
#include "enum_helpers.h"
#include "json_parser.h"
#include "platform_services.h"
#include "supervisor.h"
#include "tele.h"

#define TAG "cikon:supervisor"

static QueueHandle_t supervisor_queue;
static EventGroupHandle_t supervisor_event_group;

static supervisor_platform_adapter_t *registered_adapters[CONFIG_SUPERVISOR_MAX_ADAPTERS];
static uint8_t adapter_count = 0;

// OTA rollback validation
static bool firmware_validated = false;

// Safe mode state
static bool safe_mode_active = false;

// Forward declarations for readability - handlers/appenders defined at end of file
static const command_entry_t core_commands[];
static const tele_entry_t core_tele[];

// Interval timing configuration
static const uint32_t supervisor_intervals_ms[SUPERVISOR_INTERVAL_COUNT] = {
    [SUPERVISOR_INTERVAL_1S] = 1000,
    [SUPERVISOR_INTERVAL_2S] = 2000,
    [SUPERVISOR_INTERVAL_5S] = 5000,
    [SUPERVISOR_INTERVAL_10S] = 10000,
    [SUPERVISOR_INTERVAL_30S] = 30000,
    [SUPERVISOR_INTERVAL_60S] = 60000,
    [SUPERVISOR_INTERVAL_5M] = 5 * 60 * 1000,
    [SUPERVISOR_INTERVAL_10M] = 10 * 60 * 1000,
    [SUPERVISOR_INTERVAL_2H] = 2 * 60 * 60 * 1000,
    [SUPERVISOR_INTERVAL_12H] = 12 * 60 * 60 * 1000};

QueueHandle_t supervisor_get_queue(void) { return supervisor_queue; }

EventGroupHandle_t supervisor_get_event_group(void) { return supervisor_event_group; }

void supervisor_notify_event(EventBits_t bits) {
    if (supervisor_event_group) {
        xEventGroupSetBits(supervisor_event_group, bits);
    }
}

esp_err_t supervisor_register_adapter(supervisor_platform_adapter_t *adapter) {
    if (adapter_count >= CONFIG_SUPERVISOR_MAX_ADAPTERS) {
        ESP_LOGE(TAG, "Maximum number of adapters (%d) reached!", CONFIG_SUPERVISOR_MAX_ADAPTERS);
        return ESP_ERR_NO_MEM;
    }

    if (!adapter) {
        ESP_LOGE(TAG, "Null adapter provided!");
        return ESP_ERR_INVALID_ARG;
    }

    registered_adapters[adapter_count++] = adapter;

    if (adapter->tele_group) {
        tele_register_group(adapter->tele_group);
    }

    if (adapter->cmnd_group) {
        cmnd_register_group(adapter->cmnd_group);
    }

    return ESP_OK;
}

const supervisor_platform_adapter_t **supervisor_get_adapters(void) {
    // Return NULL-terminated array
    static supervisor_platform_adapter_t
        *adapters_with_sentinel[CONFIG_SUPERVISOR_MAX_ADAPTERS + 1];

    for (uint8_t i = 0; i < adapter_count; i++) {
        adapters_with_sentinel[i] = registered_adapters[i];
    }
    adapters_with_sentinel[adapter_count] = NULL; // Sentinel

    return (const supervisor_platform_adapter_t **)adapters_with_sentinel;
}

bool supervisor_is_safe_mode_active(void) { return safe_mode_active; }

// Safe mode implementation - inspired by ESPHome safe mode mechanism
// https://github.com/esphome/esphome/blob/dev/esphome/components/safe_mode/safe_mode.cpp
// Detects repeated crashes/panics and automatically clears after stable operation
static bool safe_mode_check(void) {

    esp_reset_reason_t reason = esp_reset_reason();
    uint32_t boot_counter = config_get()->boot_counter;

    if (is_abnormal_reset(reason)) {

        boot_counter++;
        ESP_LOGW(TAG, "Crash detected (%s), boot counter: %u/%u",
                 esp_reset_reason_to_string(reason), boot_counter,
                 CONFIG_SUPERVISOR_SAFE_MODE_THRESHOLD);
        config_set_boot_counter(boot_counter);
    }

    if (boot_counter >= CONFIG_SUPERVISOR_SAFE_MODE_THRESHOLD) {
        ESP_LOGE(TAG, "Safe mode active: %u crashes detected", boot_counter);
        ESP_LOGE(TAG, "Hardware adapters DISABLED - WiFi/OTA only");
        ESP_LOGE(TAG, "Auto-clear after %ds stable operation",
                 CONFIG_SUPERVISOR_SAFE_MODE_STABLE_TIME_S);
        return true;
    }

    ESP_LOGI(TAG, "Boot counter: %u/%u (reset reason: %s)", boot_counter,
             CONFIG_SUPERVISOR_SAFE_MODE_THRESHOLD, esp_reset_reason_to_string(reason));
    return false;
}

static void safe_mode_clear(void) {
    config_set_boot_counter(0);

    if (safe_mode_active) {
        ESP_LOGI(TAG, "Boot counter cleared - restart to exit safe mode");
    } else {
        ESP_LOGI(TAG, "Boot counter cleared after stable operation");
    }
}

static void supervisor_validate_firmware(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        ESP_LOGD(TAG, "Failed to get OTA state");
        firmware_validated = true;
        return;
    }

    ESP_LOGI(TAG, "OTA state: %s (%d)", esp_ota_state_to_string(ota_state), ota_state);

    if (ota_state != ESP_OTA_IMG_PENDING_VERIFY) {
        // ESP_LOGI(TAG, "Firmware validation not required (state: %s)", state_str);
        firmware_validated = true;
        return;
    }

    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to validate firmware!");
        return;
    }

    esp_app_desc_t app_desc;
    if (esp_ota_get_partition_description(running, &app_desc) == ESP_OK) {
        ESP_LOGI(TAG, "✅ Firmware validated: %s v%s", app_desc.project_name, app_desc.version);
        ESP_LOGI(TAG, "   Compiled: %s %s (IDF %s)", app_desc.date, app_desc.time,
                 app_desc.idf_ver);
    }
    firmware_validated = true;
}

static void supervisor_on_interval(supervisor_interval_stage_t stage) {

    if (stage == SUPERVISOR_INTERVAL_10S && !firmware_validated) {
        supervisor_validate_firmware();
    }

    // Auto-clear boot counter after stable operation (check every 5s)
    // This prevents false positives from sporadic crashes spread over time
    if (stage == SUPERVISOR_INTERVAL_5S && config_get()->boot_counter > 0 &&
        (esp_timer_get_time() / 1000000ULL) > CONFIG_SUPERVISOR_SAFE_MODE_STABLE_TIME_S) {
        safe_mode_clear();
    }
}

// Supervisor task - core event loop
static void supervisor_task(void *args) {
    ESP_LOGI(TAG, "Supervisor task started with %d adapter(s)", adapter_count);

    TickType_t last_stage[SUPERVISOR_INTERVAL_COUNT];
    for (int i = 0; i < SUPERVISOR_INTERVAL_COUNT; ++i) {
        last_stage[i] = xTaskGetTickCount();
    }

    command_job_t *job;

    // Super loop
    while (1) {
        if (xQueueReceive(supervisor_queue, &job, pdMS_TO_TICKS(100))) {
            ESP_LOGI(TAG, "Received command: %s", job->cmnd->command_id);

            job->cmnd->handler(job->args_json_str);
            supervisor_notify_event(SUPERVISOR_EVENT_CMND_COMPLETED);

            free(job->args_json_str);
            free(job);
        }

        // Forward events to all registered adapters
        EventBits_t bits = xEventGroupGetBits(supervisor_event_group);
        if (bits) {
            xEventGroupClearBits(supervisor_event_group, bits);

            for (int i = 0; i < adapter_count; i++) {
                if (registered_adapters[i]->on_event) {
                    registered_adapters[i]->on_event(bits);
                }
            }
        }

        // Execute cyclic intervals for all registered adapters
        TickType_t now = xTaskGetTickCount();
        for (int stage = 0; stage < SUPERVISOR_INTERVAL_COUNT; stage++) {
            if (now - last_stage[stage] >= pdMS_TO_TICKS(supervisor_intervals_ms[stage])) {

                supervisor_on_interval((supervisor_interval_stage_t)stage);

                last_stage[stage] = now;

                // Safe mode: Skip forwarding intervals to adapters
                if (safe_mode_active) {
                    continue;
                }

                // Forward interval to all adapters
                for (int i = 0; i < adapter_count; i++) {
                    if (registered_adapters[i]->on_interval) {
                        registered_adapters[i]->on_interval((supervisor_interval_stage_t)stage);
                    }
                }
            }
        }
    }
}

void supervisor_init(void) {
    ESP_LOGI(TAG, "Initializing supervisor core");

    core_system_init();
    config_manager_init();

    static StaticQueue_t supervisor_queue_storage;
    static uint8_t
        supervisor_queue_buffer[CONFIG_SUPERVISOR_QUEUE_LENGTH * sizeof(command_job_t *)];

    supervisor_queue = xQueueCreateStatic(CONFIG_SUPERVISOR_QUEUE_LENGTH, sizeof(command_job_t *),
                                          supervisor_queue_buffer, &supervisor_queue_storage);

    if (!supervisor_queue) {
        ESP_LOGE(TAG, "Failed to create supervisor dispatcher queue!");
        return;
    }

    static StaticEventGroup_t supervisor_event_group_storage;

    if (supervisor_event_group == NULL) {
        supervisor_event_group = xEventGroupCreateStatic(&supervisor_event_group_storage);
    }

    if (!supervisor_event_group) {
        ESP_LOGE(TAG, "Failed to create supervisor event group!");
        return;
    }

    cmnd_init(supervisor_queue);
    cmnd_register_group(core_commands);

    tele_init();
    tele_register_group(core_tele);

    ESP_LOGI(TAG, "Supervisor core initialized successfully");
}

esp_err_t supervisor_platform_init(void) {
    ESP_LOGI(TAG, "Initializing %d platform adapter(s)", adapter_count);

    // Check safe mode before initializing adapters
    safe_mode_active = safe_mode_check();

    // Safe mode: Validate firmware immediately to allow OTA recovery
    if (safe_mode_active) {
        ESP_LOGW(TAG, "Safe mode: force validating firmware to enable OTA");
        supervisor_validate_firmware();
    }

    for (int i = 0; i < adapter_count; i++) {
        // Safe mode: Skip adapters not enabled for safe mode
        if (safe_mode_active && !registered_adapters[i]->enable_in_safe_mode) {
            ESP_LOGW(TAG, "Safe mode: skipping adapter at index %d", i);
            continue;
        }

        if (registered_adapters[i]->init) {
            registered_adapters[i]->init();
        }
    }

    xTaskCreate(supervisor_task, "supervisor", CONFIG_SUPERVISOR_TASK_STACK_SIZE, NULL,
                CONFIG_SUPERVISOR_TASK_PRIORITY, NULL);

    // Notify all adapters that platform initialization is complete
    supervisor_notify_event(SUPERVISOR_EVENT_PLATFORM_INITIALIZED);

    return ESP_OK;
}

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

static void tele_uptime_appender(const char *tele_id, cJSON *json_root) {
    uint32_t uptime = esp_timer_get_time() / 1000000ULL;
    cJSON_AddNumberToObject(json_root, tele_id, uptime);
}

static void tele_startup_appender(const char *tele_id, cJSON *json_root) {
    cJSON_AddStringToObject(json_root, tele_id, get_boot_time());
}

static void tele_onboard_led_appender(const char *tele_id, cJSON *json_root) {
    bool led_state = get_onboard_led_state();
    cJSON_AddBoolToObject(json_root, tele_id, led_state);
}

static const command_entry_t core_commands[] = {
    {"restart", "Restart the device", restart_handler},
    {"help", "Show available commands", help_handler},
    {"setconf", "Set configuration from JSON", set_conf_handler},
    {"resetconf", "Reset configuration and restart", reset_conf_handler},
    {"onboard_led", "Set onboard LED state (on/off/toggle)", onboard_led_handler},
    {NULL, NULL, NULL}};

static const tele_entry_t core_tele[] = {{"uptime", tele_uptime_appender},
                                         {"startup", tele_startup_appender},
                                         {"onboard_led", tele_onboard_led_appender},
                                         {NULL, NULL}};
