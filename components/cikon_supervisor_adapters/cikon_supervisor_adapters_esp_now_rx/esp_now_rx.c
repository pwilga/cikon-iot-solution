#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include <string.h>

#include "cikon_esp_now.h"
#include "cmnd.h"
#include "esp_now_rx_adapter.h"
#include "supervisor.h"
#include "tele.h"

#define TAG "cikon:adapter:esp_now_rx"

static bool initialized = false;
static esp_event_handler_instance_t espnow_event_handler_instance = NULL;
static uint32_t packets_received = 0;
static uint32_t packets_dropped = 0;

#define DEDUP_WINDOW_MS 500
static uint8_t last_packet_mac[6] = {0};
static uint8_t last_packet_data[32] = {0};
static size_t last_packet_len = 0;
static int64_t last_packet_time = 0;

static void esp_now_rx_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                     void *event_data) {
    if (event_base != ESP_NOW_EVENTS) {
        return;
    }

    if (event_id == ESP_NOW_EVENT_RECV_DATA) {
        esp_now_recv_event_data_t *event = (esp_now_recv_event_data_t *)event_data;

        packets_received++;

        int64_t now = esp_timer_get_time() / 1000;
        bool is_duplicate = false;

        if (now - last_packet_time < DEDUP_WINDOW_MS) {
            if (memcmp(last_packet_mac, event->src_mac, 6) == 0 &&
                last_packet_len == event->data_len &&
                memcmp(last_packet_data, event->data, event->data_len) == 0) {
                is_duplicate = true;
                packets_dropped++;
            }
        }

        if (!is_duplicate) {
            ESP_LOGI(TAG, "Received %d bytes from " MACSTR " (RSSI: %d)", event->data_len,
                     MAC2STR(event->src_mac), event->rssi);

            memcpy(last_packet_mac, event->src_mac, 6);
            last_packet_len = event->data_len < sizeof(last_packet_data) ? event->data_len
                                                                         : sizeof(last_packet_data);
            memcpy(last_packet_data, event->data, last_packet_len);
            last_packet_time = now;

            // TODO: Parse frame, decode payload, trigger actions
        }
        // else {
        //     ESP_LOGD(TAG, "Dropped duplicate frame");
        // }

        // CRITICAL: Free allocated data (posted by espnow_recv_cb)
        free(event->data);
    }
}

static void cmnd_esp_now_rx_start(const char *args_json_str) {
    if (initialized) {
        ESP_LOGW(TAG, "Already started");
        return;
    }

    ESP_LOGI(TAG, "Starting ESP-NOW RX adapter");

    esp_err_t err = espnow_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init ESP-NOW: %s", esp_err_to_name(err));
        return;
    }

    err = esp_event_handler_instance_register(ESP_NOW_EVENTS, ESP_NOW_EVENT_RECV_DATA,
                                              esp_now_rx_event_handler, NULL,
                                              &espnow_event_handler_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register event handler: %s", esp_err_to_name(err));
        espnow_shutdown();
        return;
    }

    initialized = true;
    ESP_LOGI(TAG, "ESP-NOW RX adapter started");
}

static void cmnd_esp_now_rx_stop(const char *args_json_str) {
    if (!initialized) {
        ESP_LOGW(TAG, "Not started");
        return;
    }

    ESP_LOGI(TAG, "Stopping ESP-NOW RX adapter");

    if (espnow_event_handler_instance != NULL) {
        esp_event_handler_instance_unregister(ESP_NOW_EVENTS, ESP_NOW_EVENT_RECV_DATA,
                                              espnow_event_handler_instance);
        espnow_event_handler_instance = NULL;
    }

    espnow_shutdown();
    initialized = false;

    ESP_LOGI(TAG, "ESP-NOW RX adapter stopped");
}

static void tele_esp_now_rx_stats(const char *tele_id, cJSON *json_root) {
    cJSON *stats = cJSON_CreateObject();
    cJSON_AddBoolToObject(stats, "initialized", initialized);
    cJSON_AddNumberToObject(stats, "packets_received", packets_received);
    cJSON_AddNumberToObject(stats, "packets_dropped", packets_dropped);
    cJSON_AddItemToObject(json_root, tele_id, stats);
}

static const command_entry_t esp_now_rx_commands[] = {
    {"start", "Start ESP-NOW RX", cmnd_esp_now_rx_start},
    {"stop", "Stop ESP-NOW RX", cmnd_esp_now_rx_stop},
    {NULL, NULL, NULL}};

static const tele_entry_t esp_now_rx_telemetry[] = {{"stats", tele_esp_now_rx_stats}, {NULL, NULL}};

static void esp_now_rx_adapter_init(void) {
    ESP_LOGI(TAG, "Initializing ESP-NOW RX adapter");

    packets_received = 0;
    packets_dropped = 0;

    ESP_LOGI(TAG, "ESP-NOW RX adapter initialized (use 'start' command to activate)");
}

static void esp_now_rx_adapter_shutdown(void) {
    if (!initialized) {
        return;
    }

    ESP_LOGI(TAG, "Shutting down ESP-NOW RX adapter");

    if (espnow_event_handler_instance != NULL) {
        esp_event_handler_instance_unregister(ESP_NOW_EVENTS, ESP_NOW_EVENT_RECV_DATA,
                                              espnow_event_handler_instance);
        espnow_event_handler_instance = NULL;
    }

    espnow_shutdown();
    initialized = false;

    ESP_LOGI(TAG, "ESP-NOW RX adapter shut down");
}

supervisor_platform_adapter_t esp_now_rx_adapter = {
    .init = esp_now_rx_adapter_init,
    .shutdown = esp_now_rx_adapter_shutdown,
    .on_event = NULL,
    .on_interval = NULL,
    .tele_group = esp_now_rx_telemetry,
    .cmnd_group = esp_now_rx_commands,
};
