#include "thread_commissioner.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_timer.h"
#include "openthread/commissioner.h"
#include "openthread/error.h"

#include "cmnd.h"
#include "json_parser.h"
#include "thread_common.h"

#define TAG "cikon:thread:commissioner"

// What a joiner has to know to join: 6-32 characters, upper case letters and digits, without
// I, O, Q and Z
#define THREAD_COMMISSIONER_PSKD_MIN_LEN 6

// What the commissioner was started with: the joiner entry is added once it becomes active
static char s_pskd[OT_JOINER_MAX_PSKD_LENGTH + 1];
static uint32_t s_timeout_s;
static uint32_t s_expires_s; // uptime second at which the joiner entry runs out
static uint8_t s_state = OT_COMMISSIONER_STATE_DISABLED;

static bool pskd_valid(const char *pskd) {
    size_t len = strlen(pskd);
    if (len < THREAD_COMMISSIONER_PSKD_MIN_LEN || len > OT_JOINER_MAX_PSKD_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = pskd[i];
        bool digit = c >= '0' && c <= '9';
        bool letter = c >= 'A' && c <= 'Z' && c != 'I' && c != 'O' && c != 'Q' && c != 'Z';
        if (!digit && !letter) {
            return false;
        }
    }
    return true;
}

// The commissioner callbacks run in the OpenThread task, which already holds its lock
static void commissioner_state_cb(otCommissionerState state, void *context) {
    s_state = state;

    if (state == OT_COMMISSIONER_STATE_ACTIVE) {
        // NULL joiner: whoever knows the PSKd
        otError err =
            otCommissionerAddJoiner(esp_openthread_get_instance(), NULL, s_pskd, s_timeout_s);
        if (err == OT_ERROR_NONE) {
            s_expires_s = (uint32_t)(esp_timer_get_time() / 1000000) + s_timeout_s;
            ESP_LOGI(TAG, "Commissioner active, joiners with the PSKd can join for %u s",
                     (unsigned)s_timeout_s);
        } else {
            ESP_LOGE(TAG, "Commissioner add joiner failed: %s", otThreadErrorToString(err));
        }
    } else if (state == OT_COMMISSIONER_STATE_DISABLED) {
        ESP_LOGI(TAG, "Commissioner stopped");
    }
}

static void commissioner_joiner_cb(otCommissionerJoinerEvent event, const otJoinerInfo *info,
                                   const otExtAddress *joiner_id, void *context) {
    static const char *const names[] = {"start", "connected", "finalize", "end", "entry removed"};

    // The joiner ID is there only while a session is on, not for an entry that just ran out
    char id[2 * sizeof(joiner_id->m8) + 1] = "";
    if (joiner_id) {
        for (size_t i = 0; i < sizeof(joiner_id->m8); i++) {
            snprintf(&id[2 * i], 3, "%02x", joiner_id->m8[i]);
        }
    }

    ESP_LOGI(TAG, "Joiner %s%s%s",
             (size_t)event < sizeof(names) / sizeof(names[0]) ? names[event] : "?",
             joiner_id ? " " : "", id);

    // The window ran out: stopping from the supervisor, not from inside this callback
    if (event == OT_COMMISSIONER_JOINER_REMOVED) {
        cmnd_submit("commissioner", "false");
    }
}

void thread_commissioner_handler(const char *args_json_str) {
    char pskd[OT_JOINER_MAX_PSKD_LENGTH + 1] = "";
    uint32_t timeout_s = 0;
    bool start = false;

    // {"pskd": "...", "timeout": seconds} starts the commissioner, off stops it
    cJSON *args = json_str_as_object(args_json_str);
    if (args) {
        const cJSON *pskd_item = cJSON_GetObjectItemCaseSensitive(args, "pskd");
        const cJSON *timeout_item = cJSON_GetObjectItemCaseSensitive(args, "timeout");
        bool valid = cJSON_IsString(pskd_item) && pskd_valid(pskd_item->valuestring) &&
                     cJSON_IsNumber(timeout_item) && timeout_item->valueint > 0;

        if (valid) {
            snprintf(pskd, sizeof(pskd), "%s", pskd_item->valuestring);
            timeout_s = (uint32_t)timeout_item->valueint;
        }
        cJSON_Delete(args);

        if (!valid) {
            ESP_LOGW(TAG, "Commissioner: needs a PSKd (6-32 upper case letters and digits, not I O "
                          "Q Z) and a timeout in seconds");
            return;
        }
        start = true;
    } else if (json_str_as_logic_state(args_json_str) != STATE_OFF) {
        ESP_LOGW(TAG, "Commissioner: starts with a PSKd and a timeout, stops with off");
        return;
    }

    // The lock is not ready until the OpenThread stack has started
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "Commissioner: OpenThread is not available");
        return;
    }

    otInstance *instance = esp_openthread_get_instance();

    otError err =
        start ? otCommissionerStart(instance, commissioner_state_cb, commissioner_joiner_cb, NULL)
              : otCommissionerStop(instance);

    // Before the lock is released: the state callback that adds the joiner entry reads these
    if (start && err == OT_ERROR_NONE) {
        snprintf(s_pskd, sizeof(s_pskd), "%s", pskd);
        s_timeout_s = timeout_s;
    }
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "Commissioner %s: %s", start ? "start" : "stop", otThreadErrorToString(err));
    }
}

void thread_commissioner_tele(cJSON *thread) {
    static const char *const state_names[] = {"disabled", "petition", "active"};

    cJSON *commissioner = cJSON_AddObjectToObject(thread, "commissioner");
    uint8_t state = s_state;

    cJSON_AddStringToObject(
        commissioner, "state",
        state < sizeof(state_names) / sizeof(state_names[0]) ? state_names[state] : "?");

    if (state == OT_COMMISSIONER_STATE_ACTIVE) {
        uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
        cJSON_AddNumberToObject(commissioner, "remaining",
                                s_expires_s > now_s ? s_expires_s - now_s : 0);
    }
}
