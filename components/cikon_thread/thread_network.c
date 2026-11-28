#include "thread_network.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/error.h"
#include "openthread/srp_client.h" // IWYU pragma: keep
#include "openthread/thread.h"

#include "json_parser.h"
#include "platform_services.h"
#include "thread_joiner.h"

#define TAG "cikon:thread:network"

// Long enough for a message to reach the SRP server, and for a detach, which takes up to a second
#define FORGET_STEP_MS 1500

// A task of its own, with a stack of its own: called straight from app_main on the border router,
// esp_openthread_auto_start overflowed the stack of the main task
static void thread_network_start_task(void *arg) {
    const char *pskd = arg;

    esp_openthread_lock_acquire(portMAX_DELAY);

    otOperationalDatasetTlvs dataset;

    if (otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset) == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "Using dataset from NVS");
        esp_openthread_auto_start(&dataset);
    } else {
        ESP_LOGW(TAG, "No dataset, joining through a commissioner with the device id as PSKd");
        thread_joiner_start(pskd);
    }

    esp_openthread_lock_release();

    vTaskDelete(NULL);
}

void thread_network_start(const char *pskd) {
    if (xTaskCreate(thread_network_start_task, "thread_start", 6144, (void *)pskd, 5, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "Start: no memory for the task");
    }
}

// A restart callback, registered by thread_forget alone. It comes after the one of the network
// adapter, which publishes the MQTT offline state while Thread still runs
static void thread_network_forget_on_restart(void) {
    esp_openthread_lock_acquire(portMAX_DELAY);

    otInstance *instance = esp_openthread_get_instance();

#if CONFIG_OPENTHREAD_SRP_CLIENT
    // The host name is freed too. With the server out of reach the entry stays there until its
    // lease runs out
    otSrpClientRemoveHostAndServices(instance, true, false);

    esp_openthread_lock_release();
    vTaskDelay(pdMS_TO_TICKS(FORGET_STEP_MS));
    esp_openthread_lock_acquire(portMAX_DELAY);
#endif

    // Tells the neighbors that the device leaves
    otError err = otThreadDetachGracefully(instance, NULL, NULL);
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "Forget: detach: %s", otThreadErrorToString(err));
    }

    esp_openthread_lock_release();
    vTaskDelay(pdMS_TO_TICKS(FORGET_STEP_MS));
    esp_openthread_lock_acquire(portMAX_DELAY);

    // An empty dataset erases the stored one, with Thread disabled
    const otOperationalDatasetTlvs empty = {0};
    err = otThreadSetEnabled(instance, false);
    if (err == OT_ERROR_NONE) {
        err = otDatasetSetActiveTlvs(instance, &empty);
    }
    if (err == OT_ERROR_NONE) {
        err = otDatasetSetPendingTlvs(instance, &empty);
    }

    esp_openthread_lock_release();

    if (err == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "Network forgotten, restarting");
    } else {
        ESP_LOGE(TAG, "Forget: %s, restarting", otThreadErrorToString(err));
    }
}

void thread_network_forget_handler(const char *args_json_str) {
    (void)args_json_str;

    // The lock is not ready until the OpenThread stack has started
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "Forget: OpenThread is not available");
        return;
    }

    otOperationalDatasetTlvs dataset;
    otError err = otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset);

    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "Forget: the device has no network");
        return;
    }

    register_restart_callback(thread_network_forget_on_restart);
    esp_safe_restart();
}

#if CONFIG_OPENTHREAD_FTD
void thread_network_create_handler(const char *args_json_str) {
    char name[OT_NETWORK_NAME_MAX_SIZE + 1] = "";
    int channel = 0;

    // {"name": "...", "channel": 11-26}, both are needed
    cJSON *args = json_str_as_object(args_json_str);
    if (args) {
        const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(args, "name");
        const cJSON *channel_item = cJSON_GetObjectItemCaseSensitive(args, "channel");

        if (cJSON_IsString(name_item)) {
            snprintf(name, sizeof(name), "%s", name_item->valuestring);
        }
        if (cJSON_IsNumber(channel_item)) {
            channel = channel_item->valueint;
        }
        cJSON_Delete(args);
    }

    if (name[0] == '\0' || channel < 11 || channel > 26) {
        ESP_LOGW(TAG, "Create: needs {\"name\": up to 16 characters, \"channel\": 11-26}");
        return;
    }

    // The lock is not ready until the OpenThread stack has started
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "Create: OpenThread is not available");
        return;
    }

    otInstance *instance = esp_openthread_get_instance();
    otOperationalDatasetTlvs dataset;

    if (otDatasetGetActiveTlvs(instance, &dataset) == OT_ERROR_NONE) {
        esp_openthread_lock_release();
        ESP_LOGW(TAG, "Create: the device has a network, thread_forget erases it");
        return;
    }

    otOperationalDataset config_dataset;
    otError err = otDatasetCreateNewNetwork(instance, &config_dataset);

    if (err == OT_ERROR_NONE) {
        strlcpy(config_dataset.mNetworkName.m8, name, sizeof(config_dataset.mNetworkName.m8));
        config_dataset.mComponents.mIsNetworkNamePresent = true;

        config_dataset.mChannel = (uint16_t)channel;
        config_dataset.mComponents.mIsChannelPresent = true;

        otDatasetConvertToTlvs(&config_dataset, &dataset);

        // The joiner would keep trying on a device that has its network now
        thread_joiner_stop();

        if (esp_openthread_auto_start(&dataset) != ESP_OK) {
            err = OT_ERROR_FAILED;
        }
    }

    esp_openthread_lock_release();

    if (err == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "Created new Thread dataset: %s ch%d", name, channel);
    } else {
        ESP_LOGE(TAG, "Create: %s", otThreadErrorToString(err));
    }
}
#endif // CONFIG_OPENTHREAD_FTD
