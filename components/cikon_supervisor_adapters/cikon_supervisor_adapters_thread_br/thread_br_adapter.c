#include "thread_br_adapter.h"

#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_types.h"
#include "openthread/dataset_ftd.h"
#include "openthread/instance.h"

#include "bits_helper.h"
#include "supervisor.h"

#define TAG "cikon:adapter:thread_br"

static bool initialized = false;

static const esp_openthread_config_t s_ot_config = {
    .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
    .platform_config = {
        .radio_config = {
            .radio_mode = RADIO_MODE_UART_RCP,
            .radio_uart_config = {
                .port = CONFIG_THREAD_BR_RCP_UART_PORT,
                .uart_config = {
                    .baud_rate  = CONFIG_THREAD_BR_RCP_BAUD,
                    .data_bits  = UART_DATA_8_BITS,
                    .parity     = UART_PARITY_DISABLE,
                    .stop_bits  = UART_STOP_BITS_1,
                    .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
                    .source_clk = UART_SCLK_DEFAULT,
                },
                .rx_pin = CONFIG_THREAD_BR_RCP_RX_PIN,
                .tx_pin = CONFIG_THREAD_BR_RCP_TX_PIN,
            },
        },
        .host_config = {
            .host_connection_mode = HOST_CONNECTION_MODE_NONE,
        },
        .port_config = {
            .storage_partition_name = "nvs",
            .netif_queue_size = 10,
            .task_queue_size  = 10,
        },
    },
};

static esp_err_t thread_br_adapter_init(void)
{
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // esp_netif_init() already called by ethernet/wifi adapter before us
    ESP_LOGI(TAG, "Starting OpenThread stack");
    esp_err_t err = esp_openthread_start(&s_ot_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_openthread_start failed: %s", esp_err_to_name(err));
        return err;
    }

    initialized = true;
    ESP_LOGI(TAG, "OpenThread stack started, waiting for backbone IP");
    return ESP_OK;
}

static esp_err_t thread_br_adapter_shutdown(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (initialized) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        esp_openthread_border_router_deinit();
        esp_openthread_lock_release();
        initialized = false;
    }
    initialized = false;
    return ESP_OK;
}

static void thread_br_adapter_on_event(EventBits_t bits)
{
    const EventBits_t backbone_ready = INET_ETH_READY | INET_EVENT_STA_READY;
    if (!(bits & backbone_ready) || initialized) {
        return;
    }

    esp_netif_t *backbone = esp_netif_get_default_netif();
    if (!backbone) {
        ESP_LOGE(TAG, "No backbone netif");
        return;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);

    esp_openthread_set_backbone_netif(backbone);

    if (esp_openthread_border_router_init() != ESP_OK) {
        ESP_LOGE(TAG, "Border router init failed");
        esp_openthread_lock_release();
        return;
    }

    otOperationalDatasetTlvs dataset;
    bool dataset_ready = false;

#if defined(CONFIG_THREAD_BR_PROVISIONED_DATASET) && strlen(CONFIG_THREAD_BR_PROVISIONED_DATASET) > 0
    if (otDatasetParseTlvs(CONFIG_THREAD_BR_PROVISIONED_DATASET, &dataset) == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "Using provisioned dataset");
        dataset_ready = true;
    } else {
        ESP_LOGW(TAG, "Provisioned dataset parse failed, falling back to NVS/new");
    }
#endif

    if (!dataset_ready) {
        if (otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset) == OT_ERROR_NONE) {
            ESP_LOGI(TAG, "Using dataset from NVS");
        } else {
            otOperationalDataset config_dataset;
            otDatasetCreateNewNetwork(esp_openthread_get_instance(), &config_dataset);

            memcpy(config_dataset.mNetworkName.m8, CONFIG_THREAD_BR_NETWORK_NAME,
                   strlen(CONFIG_THREAD_BR_NETWORK_NAME) + 1);
            config_dataset.mComponents.mIsNetworkNamePresent = true;

            config_dataset.mChannel = CONFIG_THREAD_BR_CHANNEL;
            config_dataset.mComponents.mIsChannelPresent = true;

#if CONFIG_THREAD_BR_WAKEUP_CHANNEL > 0
            config_dataset.mWakeupChannel = CONFIG_THREAD_BR_WAKEUP_CHANNEL;
            config_dataset.mComponents.mIsWakeupChannelPresent = true;
#endif

            otDatasetConvertToTlvs(&config_dataset, &dataset);
            ESP_LOGI(TAG, "Created new Thread dataset: %s ch%d", CONFIG_THREAD_BR_NETWORK_NAME,
                     CONFIG_THREAD_BR_CHANNEL);
        }
    }

    esp_openthread_auto_start(&dataset);
    esp_openthread_lock_release();

    initialized = true;
    supervisor_notify_event(THREAD_BR_READY);
    ESP_LOGI(TAG, "Thread Border Router started");
}

supervisor_platform_adapter_t thread_br_adapter = {
    .name             = "thread_br",
    .enable_in_safe_mode = false,
    .init             = thread_br_adapter_init,
    .shutdown         = thread_br_adapter_shutdown,
    .on_event         = thread_br_adapter_on_event,
};
