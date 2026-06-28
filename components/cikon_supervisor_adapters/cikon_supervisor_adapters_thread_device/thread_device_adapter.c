#include "thread_device_adapter.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h" // IWYU pragma: keep
#include "esp_openthread_types.h"
#include "openthread/dataset.h"
#include "openthread/error.h"
#include "openthread/link.h" // IWYU pragma: keep
#include "openthread/srp_client.h"
#include "openthread/thread.h"

#include "bits_helper.h"
#include "config_manager.h"
#include "inet_common.h"
#include "supervisor.h"
#include "thread_common.h"
#include "thread_radio_config.h"

#define TAG "cikon:adapter:thread_device"

static bool initialized = false;
static bool s_mqtt_started = false;
static const esp_openthread_config_t s_ot_config = THREAD_DEFAULT_OT_CONFIG();

#if CONFIG_OPENTHREAD_SRP_CLIENT
static otSrpClientService s_srp_http_service;

static void srp_client_callback(otError error, const otSrpClientHostInfo *hostInfo,
                                const otSrpClientService *services,
                                const otSrpClientService *removedServices, void *context) {
    (void)hostInfo;
    (void)services;
    (void)removedServices;
    (void)context;
    if (error == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "SRP: registered OK");
    } else if (error == OT_ERROR_DUPLICATED) {
        // SRP key mismatch — NVS was likely erased. Remove the stale host entry on the OBR.
        // To clear all SRP registrations on OBR: ot srp server disable && ot srp server enable
        ESP_LOGE(TAG, "SRP: name conflict for %s — remove stale host on OBR",
                 inet_common_get_hostname());
    } else {
        ESP_LOGW(TAG, "SRP error: %s", otThreadErrorToString(error));
    }
}

static void srp_host_init(void) {
    const char *hostname = inet_common_get_hostname();
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    otSrpClientSetCallback(instance, srp_client_callback, NULL);
    otError err = otSrpClientSetHostName(instance, hostname);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "SRP set hostname: %s", otThreadErrorToString(err));
        esp_openthread_lock_release();
        return;
    }
    err = otSrpClientEnableAutoHostAddress(instance);
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "SRP auto host addr: %s", otThreadErrorToString(err));
    }
    otSrpClientEnableAutoStartMode(instance, NULL, NULL);
    ESP_LOGI(TAG, "SRP host: %s, lease: %lus", hostname,
             (unsigned long)otSrpClientGetLeaseInterval(instance));
    esp_openthread_lock_release();
}

static void srp_add_service(otSrpClientService *svc, const char *type, uint16_t port) {
    memset(svc, 0, sizeof(*svc));
    svc->mName = type;
    svc->mInstanceName = inet_common_get_hostname();
    svc->mPort = port;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = otSrpClientAddService(esp_openthread_get_instance(), svc);
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
        ESP_LOGE(TAG, "SRP add %s: %s", type, otThreadErrorToString(err));
    } else {
        ESP_LOGI(TAG, "SRP: %s port %d", type, port);
    }
}
#endif

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

#if CONFIG_OPENTHREAD_FTD
    otLinkModeConfig mode = {.mRxOnWhenIdle = true, .mDeviceType = true, .mNetworkData = true};
    otError err = otThreadSetLinkMode(esp_openthread_get_instance(), mode);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otThreadSetLinkMode: %s", otThreadErrorToString(err));
    }
#elif CONFIG_OPENTHREAD_MTD
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

static void thread_device_on_dns_server_ready(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (s_mqtt_started) {
        return;
    }
    s_mqtt_started = true;
#if CONFIG_OPENTHREAD_SRP_CLIENT
    srp_host_init();
#endif
    inet_common_sntp_init();
    inet_common_mqtt_init();
    inet_common_http_init();
#if CONFIG_OPENTHREAD_SRP_CLIENT
    srp_add_service(&s_srp_http_service, "_http._tcp", CONFIG_HTTP_PORT);
#endif
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

    esp_event_handler_register(OPENTHREAD_EVENT, OPENTHREAD_EVENT_SET_DNS_SERVER,
                               thread_device_on_dns_server_ready, NULL);

    xTaskCreate(thread_device_start_task, "td_start", 4096, NULL, 5, NULL);
    return ESP_OK;
}

static void thread_device_adapter_on_event(EventBits_t bits) { inet_common_on_event(bits); }

static esp_err_t thread_device_adapter_shutdown(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    initialized = false;
    return ESP_OK;
}

static const command_entry_t s_thread_device_cmnd[] = {
    {"ota", "Control OTA service (on/off)", inet_common_ota_handler},
    {"monitor", "Control TCP monitor (on/off)", inet_common_monitor_handler},
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    {"ha", "Trigger HA MQTT discovery", inet_common_ha_discovery_handler},
#endif
    {NULL, NULL, NULL},
};

supervisor_platform_adapter_t thread_device_adapter = {
    .name = "thread_device",
    .enable_in_safe_mode = false,
    .init = thread_device_adapter_init,
    .shutdown = thread_device_adapter_shutdown,
    .on_event = thread_device_adapter_on_event,
    .cmnd_group = s_thread_device_cmnd,
};
