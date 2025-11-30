#include <stdint.h>

#include "cJSON.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "platform_services.h"
#include "tele.h"
#include "tele_core_appenders.h"

float random_float(float min, float max) {
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

/**
 * @brief Generates a JSON object describing current FreeRTOS tasks.
 *
 * This function collects runtime information about all currently active
 * FreeRTOS tasks and serializes it into a JSON dictionary using cJSON.
 * Each task is represented as a sub-object with fields such as priority,
 * stack watermark, runtime counter, task number, state, and core affinity
 * (if supported).
 *
 * @note Intended for diagnostic/debug use — prioritizes clarity over
 * efficiency.
 *
 * @return cJSON* Root JSON object representing the task dictionary,
 *                or NULL on allocation failure.
 */

void tele_tasks_dict_appender(const char *tele_id, cJSON *json_root) {

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

void tele_core_appenders_register(void) {

    tele_register("uptime", tele_uptime_appender);
    tele_register("startup", tele_startup_appender);
    tele_register("temperature", tele_temperature_appender);
    tele_register("onboard_led", tele_onboard_led_appender);

    tele_register("tasks_dict", tele_tasks_dict_appender);
}