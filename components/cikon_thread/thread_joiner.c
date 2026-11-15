#include "thread_joiner.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_timer.h"
#include "openthread/commissioner.h" // OT_JOINER_MAX_PSKD_LENGTH
#include "openthread/error.h"
#include "openthread/ip6.h"
#include "openthread/joiner.h"
#include "openthread/thread.h"

#define TAG "cikon:thread:joiner"

#define JOINER_RETRY_US (30 * 1000000LL)

static char s_pskd[OT_JOINER_MAX_PSKD_LENGTH + 1];
static esp_timer_handle_t s_retry_timer;

static void joiner_attempt(void);

// Runs in the OpenThread task, which already holds its lock
static void joiner_cb(otError error, void *context) {
    otInstance *instance = esp_openthread_get_instance();

    if (error == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "Joined, starting Thread");
        otError err = otThreadSetEnabled(instance, true);
        if (err != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "otThreadSetEnabled: %s", otThreadErrorToString(err));
        }
        return;
    }

    ESP_LOGW(TAG, "Join failed: %s, retry in %d s", otThreadErrorToString(error),
             (int)(JOINER_RETRY_US / 1000000));
    esp_timer_start_once(s_retry_timer, JOINER_RETRY_US);
}

// Called with the OpenThread lock held
static void joiner_attempt(void) {
    otInstance *instance = esp_openthread_get_instance();

    otError err = otIp6SetEnabled(instance, true);
    if (err == OT_ERROR_NONE) {
        err = otJoinerStart(instance, s_pskd, NULL, NULL, NULL, NULL, NULL, joiner_cb, NULL);
    }
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "Joiner start: %s, retry in %d s", otThreadErrorToString(err),
                 (int)(JOINER_RETRY_US / 1000000));
        esp_timer_start_once(s_retry_timer, JOINER_RETRY_US);
        return;
    }
    ESP_LOGI(TAG, "Looking for a commissioner");
}

// Runs in the esp_timer task, not in the OpenThread one
static void joiner_retry_timer_cb(void *arg) {
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        esp_timer_start_once(s_retry_timer, JOINER_RETRY_US);
        return;
    }
    joiner_attempt();
    esp_openthread_lock_release();
}

void thread_joiner_start(const char *pskd) {
    if (!s_retry_timer) {
        const esp_timer_create_args_t args = {.callback = joiner_retry_timer_cb,
                                              .name = "td_joiner"};
        if (esp_timer_create(&args, &s_retry_timer) != ESP_OK) {
            ESP_LOGE(TAG, "Retry timer create failed");
            return;
        }
    }

    snprintf(s_pskd, sizeof(s_pskd), "%s", pskd);
    joiner_attempt();
}
