#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_dns64.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h" // IWYU pragma: keep
#include "esp_openthread_types.h"
#include "openthread/dataset.h"
#include "openthread/error.h"
#include "openthread/link.h" // IWYU pragma: keep
#include "openthread/thread.h"

#include "bits_helper.h"
#include "config_manager.h"
#include "supervisor.h"
#include "tele.h"
#include "thread_common.h"
#include "thread_radio_config.h"
#include "uplink.h"

#define TAG "cikon:network:thread"

static const esp_openthread_config_t s_ot_config = THREAD_DEFAULT_OT_CONFIG();

static esp_event_handler_instance_t ot_event_instance = NULL;
static bool dns_known = false; // the border router has advertised its way out (NAT64 DNS)

static void uplink_thread_start_task(void *arg) {
    esp_openthread_lock_acquire(portMAX_DELAY);

    otOperationalDatasetTlvs dataset;
    bool dataset_ready = false;

    if (CONFIG_THREAD_DEVICE_PROVISIONED_DATASET[0] != '\0') {
        if (thread_dataset_parse_hex(CONFIG_THREAD_DEVICE_PROVISIONED_DATASET, &dataset)) {
            ESP_LOGI(TAG, "Using provisioned dataset");
            dataset_ready = true;
        } else {
            ESP_LOGW(TAG, "Provisioned dataset parse failed, trying NVS");
        }
    }

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
#endif

    if (dataset_ready) {
        esp_openthread_auto_start(&dataset);
    }

    esp_openthread_lock_release();

    ESP_LOGI(TAG, "Thread device started");

    vTaskDelete(NULL);
}

static void uplink_thread_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                        void *event_data) {
    switch (event_id) {
    case OPENTHREAD_EVENT_SET_DNS_SERVER:
        dns_known = true;
        ESP_LOGI(TAG, "Border router provides a way out");
        supervisor_notify_event(NETWORK_UPLINK_READY);
        break;
    case OPENTHREAD_EVENT_ATTACHED:
        // Rejoining keeps the same DNS, so SET_DNS_SERVER doesn't come again
        if (dns_known) {
            supervisor_notify_event(NETWORK_UPLINK_READY);
        }
        break;
    case OPENTHREAD_EVENT_DETACHED:
        ESP_LOGW(TAG, "Detached from the Thread network");
        supervisor_notify_event(NETWORK_UPLINK_LOST);
        break;
    }
}

static esp_err_t uplink_thread_start(void) {

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

    // Force-link IDF's OpenThread DNS64/NAT64 lwIP glue (esp_openthread_dns64.c). Without an
    // explicit reference the linker GCs the whole TU, dropping lwip_hook_dns_external_resolve —
    // then lwIP sockets (MQTT/SNTP/esp-tls) can't reach NAT64 while `ot ping` still works. Requires
    // CONFIG_LWIP_HOOK_DNS_EXT_RESOLVE_CUSTOM=y so lwIP actually calls the hook.
    esp_openthread_dns64_client_init();

#if CONFIG_OPENTHREAD_CLI
    thread_cli_commands_init();
#endif

    ESP_ERROR_CHECK(esp_event_handler_instance_register(OPENTHREAD_EVENT, ESP_EVENT_ANY_ID,
                                                        &uplink_thread_event_handler, NULL,
                                                        &ot_event_instance));

    xTaskCreate(uplink_thread_start_task, "td_start", 4096, NULL, 5, NULL);
    return ESP_OK;
}

// The OpenThread stack keeps running
static esp_err_t uplink_thread_stop(void) {

    if (ot_event_instance) {
        esp_event_handler_instance_unregister(OPENTHREAD_EVENT, ESP_EVENT_ANY_ID,
                                              ot_event_instance);
        ot_event_instance = NULL;
    }

    return ESP_OK;
}

static void uplink_thread_tele_ip_address(const char *tele_id, cJSON *json_root) {
    char ip[OT_IP6_ADDRESS_STRING_SIZE];
    if (thread_get_reachable_ip6(ip, sizeof(ip))) {
        cJSON_AddStringToObject(json_root, tele_id, ip);
    }
}

static const tele_entry_t uplink_thread_tele[] = {{"ip", uplink_thread_tele_ip_address},
                                                  {NULL, NULL}};

const uplink_t uplink_thread = {
    .name = "thread",
    .start = uplink_thread_start,
    .stop = uplink_thread_stop,
    .commands = NULL,
    .tele = uplink_thread_tele,
};
