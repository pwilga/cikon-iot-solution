#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"

#include "cikon_esp_now.h"

#define TAG "cikon:esp_now"

ESP_EVENT_DEFINE_BASE(ESP_NOW_EVENTS);

static bool initialized = false;
static bool wifi_minimal_mode = false;
static esp_event_handler_instance_t wifi_event_handler_instance = NULL;

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {

    esp_now_send_event_data_t event_data = {.success = (status == ESP_NOW_SEND_SUCCESS)};
    memcpy(event_data.mac_addr, tx_info->des_addr, 6);

    esp_event_post(ESP_NOW_EVENTS, ESP_NOW_EVENT_SEND_DONE, &event_data, sizeof(event_data), 0);
}

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    esp_now_recv_event_data_t event_data = {
        .data = malloc(len), .data_len = len, .rssi = info->rx_ctrl->rssi};

    if (event_data.data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for RX event");
        return;
    }

    memcpy(event_data.src_mac, info->src_addr, 6);
    memcpy(event_data.data, data, len);

    esp_err_t err =
        esp_event_post(ESP_NOW_EVENTS, ESP_NOW_EVENT_RECV_DATA, &event_data, sizeof(event_data), 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post RX event: %s", esp_err_to_name(err));
        free(event_data.data);
    }
}

static void espnow_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                      void *event_data) {
    if (event_base != WIFI_EVENT) {
        return;
    }

    if (event_id == WIFI_EVENT_STA_START || event_id == WIFI_EVENT_AP_START) {

        // If we were in minimal mode and WiFi is now started by full stack,
        // switch to full mode and allow future reinits
        if (wifi_minimal_mode) {
            ESP_LOGI(TAG, "WiFi transitioned from minimal to full mode");
            wifi_minimal_mode = false;
        }

        ESP_LOGI(TAG, "WiFi mode changed, reinitializing ESP-NOW");

        esp_now_deinit();

        esp_err_t err = esp_now_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reinit ESP-NOW: %s", esp_err_to_name(err));
            initialized = false;
            return;
        }

        // Re-register callbacks
        esp_now_register_send_cb(espnow_send_cb);
        esp_now_register_recv_cb(espnow_recv_cb);

#ifdef CONFIG_ESPNOW_ENABLE_ENCRYPTION
        esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK);
#endif

        // ESP_LOGI(TAG, "ESP-NOW reinitialized after WiFi mode change");
    }
}

esp_err_t espnow_init(void) {
    if (initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing ESP-NOW");

    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);

    if (err == ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGI(TAG, "WiFi not initialized, starting minimal WiFi for ESP-NOW");

        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to init netif: %s", esp_err_to_name(err));
            return err;
        }

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init WiFi: %s", esp_err_to_name(err));
            return err;
        }

        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(err));
            return err;
        }

        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
            return err;
        }

        wifi_minimal_mode = true;
        ESP_LOGI(TAG, "Minimal WiFi initialized (STA mode, no connection)");

    } else if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi already initialized (mode: %d), using existing driver", mode);
        wifi_minimal_mode = false;
    } else {
        ESP_LOGE(TAG, "Failed to get WiFi mode: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_now_register_send_cb(espnow_send_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Register send callback failed: %s", esp_err_to_name(err));
        goto cleanup_espnow;
    }

    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Register recv callback failed: %s", esp_err_to_name(err));
        goto cleanup_espnow;
    }

#ifdef CONFIG_ESPNOW_ENABLE_ENCRYPTION
    err = esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Set PMK failed: %s", esp_err_to_name(err));
    }
#endif

    err =
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, espnow_wifi_event_handler,
                                            NULL, &wifi_event_handler_instance);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(err));
    }

    initialized = true;
    // ESP_LOGI(TAG, "ESP-NOW initialized");

    return ESP_OK;

cleanup_espnow:
    esp_now_deinit();
    return err;
}

esp_err_t espnow_shutdown(void) {
    if (!initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Shutting down ESP-NOW");

    if (wifi_event_handler_instance != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler_instance);
        wifi_event_handler_instance = NULL;
    }

    esp_err_t err = esp_now_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_deinit failed: %s", esp_err_to_name(err));
    }

    initialized = false;
    // ESP_LOGI(TAG, "ESP-NOW shut down");

    return err;
}
