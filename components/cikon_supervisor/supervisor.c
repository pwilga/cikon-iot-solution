#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cmnd.h"
#include "cmnd_core_handlers.h"
#include "config_manager.h"
#include "debug.h"
#include "platform_services.h"
#include "supervisor.h"
#include "tele.h"
#include "tele_core_appenders.h"

#define TAG "cikon-supervisor"
#define SUPERVISOR_MAX_ADAPTERS 4

static QueueHandle_t supervisor_queue;
static EventGroupHandle_t supervisor_event_group;

static supervisor_platform_adapter_t *registered_adapters[SUPERVISOR_MAX_ADAPTERS];
static uint8_t adapter_count = 0;

// Interval timing configuration
static const uint32_t supervisor_intervals_ms[SUPERVISOR_INTERVAL_COUNT] = {
    [SUPERVISOR_INTERVAL_1S] = 1000,
    [SUPERVISOR_INTERVAL_5S] = 5000,
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
    ESP_LOGI(TAG, "Registered adapter #%d", adapter_count);

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
    cmnd_core_handlers_register();

    tele_init();
    tele_core_appenders_register();

    xTaskCreate(debug_info_task, "debug_info", 4096, NULL, 0, NULL);

    ESP_LOGI(TAG, "Supervisor core initialized successfully");
}

esp_err_t supervisor_platform_init(void) {
    ESP_LOGI(TAG, "Initializing %d platform adapter(s)", adapter_count);

    for (int i = 0; i < adapter_count; i++) {
        if (registered_adapters[i]->init) {
            ESP_LOGI(TAG, "Initializing adapter #%d", i + 1);
            registered_adapters[i]->init();
        }
    }

    xTaskCreate(supervisor_task, "supervisor", CONFIG_SUPERVISOR_TASK_STACK_SIZE, NULL,
                CONFIG_SUPERVISOR_TASK_PRIORITY, NULL);

    // ESP_LOGI(TAG, "Supervisor platform initialization complete");

    return ESP_OK;
}
