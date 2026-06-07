#include "thread_device_adapter.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h" // IWYU pragma: keep
#include "esp_openthread_types.h"
#include "openthread/dataset.h"
#include "openthread/link.h"

#include "bits_helper.h"
#include "config_manager.h"
#include "supervisor.h"
#include "thread_common.h"
#include "thread_radio_config.h"

#define TAG "cikon:adapter:thread_device"

static bool initialized = false;

static const esp_openthread_config_t s_ot_config = THREAD_DEFAULT_OT_CONFIG();

static void thread_device_start_task(void *arg) {
    esp_openthread_lock_acquire(portMAX_DELAY);

    otOperationalDatasetTlvs dataset;
    bool dataset_ready = false;

#ifdef CONFIG_THREAD_DEVICE_PROVISIONED_DATASET
    if (strlen(CONFIG_THREAD_DEVICE_PROVISIONED_DATASET) > 0 &&
        thread_dataset_parse_hex(CONFIG_THREAD_DEVICE_PROVISIONED_DATASET, &dataset)) {
        ESP_LOGI(TAG, "Using provisioned dataset");
        dataset_ready = true;
    } else {
        ESP_LOGW(TAG, "Provisioned dataset parse failed, trying NVS");
    }
#endif

    if (!dataset_ready) {
        if (otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset) == OT_ERROR_NONE) {
            ESP_LOGI(TAG, "Using dataset from NVS");
            dataset_ready = true;
        } else {
            ESP_LOGW(TAG, "No dataset — Thread will not join automatically");
            ESP_LOGW(TAG, "Provision via: ot dataset active -x (from BR), then ot ifconfig up / ot "
                          "thread start");
        }
    }

#if CONFIG_OPENTHREAD_MTD
    otLinkSetPollPeriod(esp_openthread_get_instance(), CONFIG_THREAD_DEVICE_POLL_PERIOD_MS);
    ESP_LOGI(TAG, "SED poll period: %d ms", CONFIG_THREAD_DEVICE_POLL_PERIOD_MS);
#endif

    if (dataset_ready) {
        esp_openthread_auto_start(&dataset);
    }

    esp_openthread_lock_release();

    initialized = true;
    supervisor_notify_event(THREAD_DEVICE_READY);
    ESP_LOGI(TAG, "Thread device started");

    vTaskDelete(NULL);
}

static esp_err_t thread_device_adapter_init(void) {
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // esp_netif_init() may not be called by any other adapter on H2/C6 (no WiFi/Ethernet)
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

#if CONFIG_OPENTHREAD_CLI
    {
        static char repl_prompt[20];
        snprintf(repl_prompt, sizeof(repl_prompt), "%s>", config_get()->dev_name);
        thread_console_start(repl_prompt);
    }
#endif

    ESP_LOGI(TAG, "Starting OpenThread stack (native radio)");
    err = esp_openthread_start(&s_ot_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_openthread_start failed: %s", esp_err_to_name(err));
        return err;
    }

#if CONFIG_OPENTHREAD_CLI
    thread_cli_commands_init();
#endif

    xTaskCreate(thread_device_start_task, "td_start", 4096, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t thread_device_adapter_shutdown(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    initialized = false;
    return ESP_OK;
}

supervisor_platform_adapter_t thread_device_adapter = {
    .name = "thread_device",
    .enable_in_safe_mode = false,
    .init = thread_device_adapter_init,
    .shutdown = thread_device_adapter_shutdown,
};
