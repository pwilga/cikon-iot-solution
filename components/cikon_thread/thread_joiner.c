#include "thread_joiner.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/commissioner.h" // OT_JOINER_MAX_PSKD_LENGTH
#include "openthread/dataset.h"
#include "openthread/error.h"
#include "openthread/ip6.h"
#include "openthread/joiner.h"
#include "openthread/thread.h"

#define TAG "cikon:thread:joiner"

#define JOINER_RETRY_MS (30 * 1000)

static char s_pskd[OT_JOINER_MAX_PSKD_LENGTH + 1];
// Read and written with the OpenThread lock held. NULL once the retries are over
static TaskHandle_t s_joiner_task;

// Runs in the OpenThread task, which already holds its lock
static void thread_joiner_cb(otError error, void *context) {
    otInstance *instance = esp_openthread_get_instance();

    if (error == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "Joined, starting Thread");
        otError err = otThreadSetEnabled(instance, true);
        if (err != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "otThreadSetEnabled: %s", otThreadErrorToString(err));
        }
    } else {
        ESP_LOGW(TAG, "Join failed: %s, retry in %d s", otThreadErrorToString(error),
                 JOINER_RETRY_MS / 1000);
    }

    if (s_joiner_task) {
        xTaskNotifyGive(s_joiner_task);
    }
}

// Called with the OpenThread lock held. True when the callback is going to report the result
static bool thread_joiner_attempt(void) {
    otInstance *instance = esp_openthread_get_instance();

    // A notification left from an earlier attempt would end the wait for this one
    ulTaskNotifyTake(pdTRUE, 0);

    otError err = otIp6SetEnabled(instance, true);
    if (err == OT_ERROR_NONE) {
        err = otJoinerStart(instance, s_pskd, NULL, NULL, NULL, NULL, NULL, thread_joiner_cb, NULL);
    }
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "Joiner start: %s, retry in %d s", otThreadErrorToString(err),
                 JOINER_RETRY_MS / 1000);
        return false;
    }
    ESP_LOGI(TAG, "Looking for a commissioner");
    return true;
}

// Tries until the device has a network, from this joiner or from elsewhere
static void thread_joiner_task(void *arg) {
    esp_openthread_lock_acquire(portMAX_DELAY);

    otOperationalDatasetTlvs dataset;

    while (otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset) != OT_ERROR_NONE) {
        bool started = thread_joiner_attempt();

        esp_openthread_lock_release();
        if (started) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        esp_openthread_lock_acquire(portMAX_DELAY);

        // No pause after a success: the task frees its stack before the services start
        if (otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset) != OT_ERROR_NONE) {
            esp_openthread_lock_release();
            // thread_joiner_stop cuts the pause short
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(JOINER_RETRY_MS));
            esp_openthread_lock_acquire(portMAX_DELAY);
        }
    }

    s_joiner_task = NULL;
    esp_openthread_lock_release();

    vTaskDelete(NULL);
}

void thread_joiner_stop(void) {
    // Stopped this way, the joiner does not call the callback, so the task is woken here
    otJoinerStop(esp_openthread_get_instance());
    if (s_joiner_task) {
        xTaskNotifyGive(s_joiner_task);
    }
}

void thread_joiner_start(const char *pskd) {
    if (s_joiner_task) {
        return;
    }

    snprintf(s_pskd, sizeof(s_pskd), "%s", pskd);

    if (xTaskCreate(thread_joiner_task, "thread_join", 6144, NULL, 5, &s_joiner_task) != pdPASS) {
        ESP_LOGE(TAG, "Start: no memory for the task");
    }
}
