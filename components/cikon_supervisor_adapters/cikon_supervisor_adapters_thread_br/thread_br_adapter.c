#include "thread_br_adapter.h"

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "openthread/dataset_ftd.h"

#include "bits_helper.h"
#include "supervisor.h"

#if CONFIG_OPENTHREAD_CLI
#include "esp_console.h"
#include "esp_ot_cli_extension.h"
#endif


#define TAG "cikon:adapter:thread_br"

static bool initialized = false;
static bool s_ot_started = false;

#ifdef CONFIG_THREAD_BR_PROVISIONED_DATASET
static bool hex_str_to_dataset_tlvs(const char *hex, otOperationalDatasetTlvs *tlvs) {
    size_t hex_len = strlen(hex);
    if (hex_len == 0 || hex_len % 2 != 0 || hex_len / 2 > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < hex_len; i += 2) {
        char byte_str[3] = {hex[i], hex[i + 1], '\0'};
        char *end;
        long val = strtol(byte_str, &end, 16);
        if (*end != '\0') {
            return false;
        }
        tlvs->mTlvs[i / 2] = (uint8_t)val;
    }
    tlvs->mLength = (uint8_t)(hex_len / 2);
    return true;
}
#endif

static const esp_openthread_config_t s_ot_config = {
    .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
    .platform_config =
        {
            .radio_config =
                {
                    .radio_mode = RADIO_MODE_UART_RCP,
                    .radio_uart_config =
                        {
                            .port = CONFIG_THREAD_BR_RCP_UART_PORT,
                            .uart_config =
                                {
                                    .baud_rate = CONFIG_THREAD_BR_RCP_BAUD,
                                    .data_bits = UART_DATA_8_BITS,
                                    .parity = UART_PARITY_DISABLE,
                                    .stop_bits = UART_STOP_BITS_1,
                                    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                                    .source_clk = UART_SCLK_DEFAULT,
                                },
                            .rx_pin = CONFIG_THREAD_BR_RCP_RX_PIN,
                            .tx_pin = CONFIG_THREAD_BR_RCP_TX_PIN,
                        },
                },
            .host_config =
                {
                    .host_connection_mode = HOST_CONNECTION_MODE_NONE,
                },
            .port_config =
                {
                    .storage_partition_name = "nvs",
                    .netif_queue_size = 10,
                    .task_queue_size = 10,
                },
        },
};

static esp_err_t thread_br_adapter_init(void) {
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // esp_netif_init() already called by ethernet/wifi adapter before us
#if CONFIG_OPENTHREAD_CLI
    {
        esp_console_repl_t *repl = NULL;
        esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
        repl_config.prompt = "ot>";
        repl_config.max_cmdline_length = 256;
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
        esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
        esp_console_dev_usb_cdc_config_t hw = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
        esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &repl_config, &repl));
#endif
        ESP_ERROR_CHECK(esp_console_start_repl(repl));
    }
#endif
    ESP_LOGI(TAG, "Starting OpenThread stack");
    esp_err_t err = esp_openthread_start(&s_ot_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_openthread_start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ot_started = true;

#if CONFIG_OPENTHREAD_CLI
    esp_cli_custom_command_init();
#endif

    ESP_LOGI(TAG, "OpenThread stack started, waiting for backbone IP");
    return ESP_OK;
}

static esp_err_t thread_br_adapter_shutdown(void) {
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


static esp_event_handler_instance_t s_ip6_handler = NULL;
static esp_netif_t *s_backbone_netif = NULL;

static void border_router_start(void) {
    esp_openthread_lock_acquire(portMAX_DELAY);

    esp_openthread_set_backbone_netif(s_backbone_netif);
    if (esp_openthread_border_router_init() != ESP_OK) {
        ESP_LOGE(TAG, "Border router init failed");
        esp_openthread_lock_release();
        return;
    }

    otOperationalDatasetTlvs dataset;
    bool dataset_ready = false;

#ifdef CONFIG_THREAD_BR_PROVISIONED_DATASET
    if (hex_str_to_dataset_tlvs(CONFIG_THREAD_BR_PROVISIONED_DATASET, &dataset)) {
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
            if (otDatasetCreateNewNetwork(esp_openthread_get_instance(), &config_dataset) !=
                OT_ERROR_NONE) {
                ESP_LOGE(TAG, "Failed to create new Thread dataset");
                esp_openthread_lock_release();
                return;
            }

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

static void border_router_start_task(void *arg) {
    border_router_start();
    vTaskDelete(NULL);
}

static void on_backbone_ip6(void *arg, esp_event_base_t base, int32_t id, void *data) {
    ip_event_got_ip6_t *evt = (ip_event_got_ip6_t *)data;
    if (evt->esp_netif != s_backbone_netif || initialized) {
        return;
    }
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_GOT_IP6, s_ip6_handler);
    s_ip6_handler = NULL;
    ESP_LOGI(TAG, "Backbone IPv6 link-local ready, starting Border Router");
    xTaskCreate(border_router_start_task, "br_start", 6144, NULL, 5, NULL);
}

static void thread_br_adapter_on_event(EventBits_t bits) {
    const EventBits_t backbone_ready = INET_ETH_READY | INET_EVENT_STA_READY;
    if (!(bits & backbone_ready) || initialized) {
        return;
    }

    if (!s_ot_started) {
        ESP_LOGW(TAG, "OpenThread not started, skipping border router init");
        return;
    }

    s_backbone_netif = esp_netif_get_default_netif();
    if (!s_backbone_netif) {
        ESP_LOGE(TAG, "No backbone netif");
        return;
    }

    esp_netif_create_ip6_linklocal(s_backbone_netif);
    ESP_LOGI(TAG, "Waiting for backbone IPv6 link-local...");
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6, on_backbone_ip6, NULL,
                                        &s_ip6_handler);
}

supervisor_platform_adapter_t thread_br_adapter = {
    .name = "thread_br",
    .enable_in_safe_mode = false,
    .init = thread_br_adapter_init,
    .shutdown = thread_br_adapter_shutdown,
    .on_event = thread_br_adapter_on_event,
};
