#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "cJSON.h"

#include "cmnd.h"

#define TAG "supervisor-cmnd"

static command_t command_registry[CONFIG_MAX_COMMANDS];
static size_t command_count = 0;
static bool cmnd_initialized = false;

static QueueHandle_t command_queue = NULL;
static bool use_immediate_execution = false;

const command_t *cmnd_get_registry(size_t *out_count) {

    if (out_count) {
        *out_count = cmnd_initialized ? command_count : 0;
    }
    if (!cmnd_initialized) {
        return NULL;
    }
    return command_registry; // read-only;
}

void cmnd_register(const char *command_id, const char *description, command_handler_t handler) {
    if (!command_id || !handler) {
        ESP_LOGE(TAG, "Invalid command registration parameters");
        return;
    }

    // if (!cmnd_initialized) {
    //     cmnd_init(NULL);
    // }

    if (command_count >= CONFIG_MAX_COMMANDS) {
        ESP_LOGE(TAG, "Command registry is full (%d commands)", CONFIG_MAX_COMMANDS);
        return;
    }

    // Check if command already exists
    for (size_t i = 0; i < command_count; i++) {
        if (strcmp(command_registry[i].command_id, command_id) == 0) {
            ESP_LOGW(TAG, "Command '%s' already registered, skipping", command_id);
            return;
        }
    }

    command_registry[command_count].command_id = command_id;
    command_registry[command_count].description = description ? description : "No description";
    command_registry[command_count].handler = handler;
    command_count++;
}

void cmnd_unregister(const char *command_id) {
    if (!command_id) {
        ESP_LOGE(TAG, "Invalid command ID for unregister");
        return;
    }

    for (size_t i = 0; i < command_count; i++) {
        if (strcmp(command_registry[i].command_id, command_id) == 0) {
            // Shift all commands after this one down
            for (size_t j = i; j < command_count - 1; j++) {
                command_registry[j] = command_registry[j + 1];
            }
            command_count--;
            ESP_LOGI(TAG, "Command '%s' unregistered", command_id);
            return;
        }
    }

    ESP_LOGW(TAG, "Command '%s' not found for unregister", command_id);
}

void cmnd_register_group(const command_entry_t *commands) {
    if (!commands) {
        ESP_LOGE(TAG, "Invalid command group parameters");
        return;
    }

    for (size_t i = 0; commands[i].command_id != NULL; i++) {
        cmnd_register(commands[i].command_id, commands[i].description, commands[i].handler);
    }
}

void cmnd_unregister_group(const command_entry_t *commands) {
    if (!commands) {
        ESP_LOGE(TAG, "Invalid command group parameters");
        return;
    }

    for (size_t i = 0; commands[i].command_id != NULL; i++) {
        cmnd_unregister(commands[i].command_id);
    }
}

const command_t *cmnd_find(const char *command_id) {

    if (!cmnd_initialized) {
        ESP_LOGE(TAG, "Command system not initialized");
        return NULL;
    }

    if (!command_id) {
        ESP_LOGE(TAG, "Command ID is NULL");
        return NULL;
    }

    // Find command in registry
    for (size_t i = 0; i < command_count; i++) {
        if (strcmp(command_registry[i].command_id, command_id) == 0) {
            return &command_registry[i];
        }
    }

    return NULL; // Command not found
}

void cmnd_init(QueueHandle_t queue) {

    if (cmnd_initialized) {
        ESP_LOGD(TAG, "Command system already initialized");
        return;
    }

    // Set execution mode
    command_queue = queue;
    use_immediate_execution = (queue == NULL);

    // Clear command registry
    memset(command_registry, 0, sizeof(command_registry));
    command_count = 0;

    cmnd_initialized = true;

    ESP_LOGI(TAG, "Command system initialized: %zu commands, mode: %s%s", command_count,
             use_immediate_execution ? "immediate execution" : "async queue",
             use_immediate_execution ? "" : (command_queue ? "" : " (NULL queue!)"));
}

void cmnd_enqueue_job(command_job_t *job) {

    if (!job) {
        ESP_LOGE(TAG, "Cannot enqueue NULL job");
        return;
    }

    if (!command_queue || xQueueSend(command_queue, &job, pdMS_TO_TICKS(100)) != pdTRUE) {

        ESP_LOGE(TAG, "Queue error: freeing resources for job [%s]",
                 (job->cmnd && job->cmnd->command_id) ? job->cmnd->command_id : "unknown");

        if (job->args_json_str) {
            free(job->args_json_str);
            job->args_json_str = NULL;
        }
        free(job);
    }
}

void cmnd_submit(const char *command_id, const char *args_json_str) {

    const command_t *cmnd = cmnd_find(command_id);

    if (!cmnd) {
        ESP_LOGW(TAG, "Unknown command: %s", command_id);
        return;
    }

    if (use_immediate_execution) {
        cmnd->handler(args_json_str);
        return;
    }

    command_job_t *job = malloc(sizeof(command_job_t));

    if (!job) {
        ESP_LOGE(TAG, "Failed to allocate memory for command job");
        return;
    }

    job->cmnd = cmnd;
    job->args_json_str = args_json_str ? strdup(args_json_str) : NULL;

    cmnd_enqueue_job(job);
}

void cmnd_process_json(const char *payload) {

    cJSON *json_root = cJSON_Parse(payload);

    if (!json_root || !cJSON_IsObject(json_root)) {
        ESP_LOGW(TAG, "Invalid JSON: Rejecting message.");
        cJSON_Delete(json_root);
        return;
    }

    for (cJSON *item = json_root->child; item != NULL; item = item->next) {
        if (!item->string) {
            continue;
        }

        char *args_json_str = cJSON_PrintUnformatted(item);
        cmnd_submit(item->string, args_json_str);
        free(args_json_str);
    }

    cJSON_Delete(json_root);
}
