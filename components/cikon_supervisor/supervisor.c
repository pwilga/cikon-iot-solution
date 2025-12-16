#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "cmnd.h"
#include "config_manager.h"
#include "json_parser.h"
#include "platform_services.h"
#include "supervisor.h"
#include "tele.h"

#define TAG "cikon:supervisor"
#define SUPERVISOR_MAX_ADAPTERS 8

static QueueHandle_t supervisor_queue;
static EventGroupHandle_t supervisor_event_group;

static supervisor_platform_adapter_t *registered_adapters[SUPERVISOR_MAX_ADAPTERS];
static uint8_t adapter_count = 0;

// Forward declarations for readability - handlers/appenders defined at end of file
static const command_entry_t core_commands[];
static const tele_entry_t core_tele[];

// Interval timing configuration
static const uint32_t supervisor_intervals_ms[SUPERVISOR_INTERVAL_COUNT] = {
    [SUPERVISOR_INTERVAL_1S] = 1000,
    [SUPERVISOR_INTERVAL_2S] = 2000,
    [SUPERVISOR_INTERVAL_5S] = 5000,
    [SUPERVISOR_INTERVAL_10S] = 10000,
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
    if (adapter_count >= SUPERVISOR_MAX_ADAPTERS) {
        ESP_LOGE(TAG, "Maximum number of adapters (%d) reached!", SUPERVISOR_MAX_ADAPTERS);
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
                // Forward interval to all adapters
                for (int i = 0; i < adapter_count; i++) {
                    if (registered_adapters[i]->on_interval) {
                        registered_adapters[i]->on_interval((supervisor_interval_stage_t)stage);
                    }
                }
                last_stage[stage] = now;
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

    for (int i = 0; i < adapter_count; i++) {
        if (registered_adapters[i]->init) {
            registered_adapters[i]->init();
        }
    }

    xTaskCreate(supervisor_task, "supervisor", CONFIG_SUPERVISOR_TASK_STACK_SIZE, NULL,
                CONFIG_SUPERVISOR_TASK_PRIORITY, NULL);

    // ESP_LOGI(TAG, "Supervisor platform initialization complete");

    return ESP_OK;
}

// ===== Core command handlers =====

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

// ===== Core telemetry appenders =====

static float random_float(float min, float max) {
    return min + ((float)esp_random() / UINT32_MAX) * (max - min);
}

static void tele_uptime_appender(const char *tele_id, cJSON *json_root) {
    uint32_t uptime = esp_timer_get_time() / 1000000ULL;
    cJSON_AddNumberToObject(json_root, tele_id, uptime);
}

static void tele_temperature_appender(const char *tele_id, cJSON *json_root) {
    float temp = random_float(20.5f, 25.9f);
    cJSON_AddNumberToObject(json_root, tele_id, temp);
}

static void tele_startup_appender(const char *tele_id, cJSON *json_root) {
    cJSON_AddStringToObject(json_root, tele_id, get_boot_time());
}

static void tele_onboard_led_appender(const char *tele_id, cJSON *json_root) {
    bool led_state = get_onboard_led_state();
    cJSON_AddBoolToObject(json_root, tele_id, led_state);
}

static void tele_tasks_dict_appender(const char *tele_id, cJSON *json_root) {
    if (!json_root)
        return;

    UBaseType_t num_tasks = uxTaskGetNumberOfTasks();
    TaskStatus_t *task_status_array = calloc(num_tasks, sizeof(TaskStatus_t));
    if (!task_status_array)
        return;

    cJSON *task_dict = cJSON_CreateObject();
    if (!task_dict) {
        free(task_status_array);
        return;
    }

    uint32_t total_runtime = 0;
    UBaseType_t real_task_count =
        uxTaskGetSystemState(task_status_array, num_tasks, &total_runtime);

    for (UBaseType_t i = 0; i < real_task_count; i++) {
        cJSON *json_task = cJSON_CreateObject();
        if (!json_task)
            continue;

        cJSON_AddNumberToObject(json_task, "prio", task_status_array[i].uxCurrentPriority);
        cJSON_AddNumberToObject(json_task, "stack", task_status_array[i].usStackHighWaterMark);
        cJSON_AddNumberToObject(json_task, "runtime_ticks", task_status_array[i].ulRunTimeCounter);
        cJSON_AddNumberToObject(json_task, "task_number", task_status_array[i].xTaskNumber);

        const char *state_str = "unknown";
        switch (task_status_array[i].eCurrentState) {
        case eRunning:
            state_str = "running";
            break;
        case eReady:
            state_str = "ready";
            break;
        case eBlocked:
            state_str = "blocked";
            break;
        case eSuspended:
            state_str = "suspended";
            break;
        case eDeleted:
            state_str = "deleted";
            break;
        default:
            break;
        }
        cJSON_AddStringToObject(json_task, "state", state_str);

#if (INCLUDE_xTaskGetAffinity == 1)
        cJSON_AddNumberToObject(json_task, "core", task_status_array[i].xCoreID);
#endif

        cJSON_AddItemToObject(task_dict, task_status_array[i].pcTaskName, json_task);
    }

    free(task_status_array);
    cJSON_AddItemToObject(json_root, tele_id, task_dict);
}

static const command_entry_t core_commands[] = {
    {"restart", "Restart the device", restart_handler},
    {"help", "Show available commands", help_handler},
    {"setconf", "Set configuration from JSON", set_conf_handler},
    {"resetconf", "Reset configuration and restart", reset_conf_handler},
    {"onboard_led", "Set onboard LED state (on/off/toggle)", onboard_led_handler},
    {NULL, NULL, NULL}};

static const tele_entry_t core_tele[] = {
    {"uptime", tele_uptime_appender},           {"startup", tele_startup_appender},
    {"temperature", tele_temperature_appender}, {"onboard_led", tele_onboard_led_appender},
    {"tasks_dict", tele_tasks_dict_appender},   {NULL, NULL}};
