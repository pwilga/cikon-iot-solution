#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <string.h>

#include "cikon_esp_now.h"
#include "esp_now_protocols.h"
#include "esp_now_rx_adapter.h"
#include "supervisor.h"
#include "tele.h"

#define TAG "cikon:adapter:esp_now_rx"

static bool initialized = false;
static esp_event_handler_instance_t espnow_event_handler_instance = NULL;
static uint32_t packets_received = 0;
static uint32_t packets_dropped = 0;

static uint32_t last_sequence = UINT32_MAX;

static esp_now_frame_callback_t user_frame_callback = NULL;

void esp_now_rx_register_callback(esp_now_frame_callback_t callback) {
    user_frame_callback = callback;
}

static bool esp_now_parse_frame(const uint8_t *data, size_t len, const uint8_t *mac, int8_t rssi,
                                esp_now_frame_t *frame) {
    if (len < 2)
        return false;

    memcpy(frame->src_mac, mac, 6);
    frame->rssi = rssi;
    frame->raw_data = data;
    frame->raw_len = len;

    if (len == sizeof(wizmote_message_t) && (data[0] == 0x81 || data[0] == 0x91)) {
        const wizmote_message_t *wizmote = (const wizmote_message_t *)data;
        frame->type = ESP_NOW_FRAME_TYPE_WIZMOTE;
        frame->sequence = wizmote->seq[0] | (wizmote->seq[1] << 8) | (wizmote->seq[2] << 16) |
                          (wizmote->seq[3] << 24);
        frame->parsed.wizmote.button_id = wizmote->button;
        frame->parsed.wizmote.battery_level = wizmote->battery_level;
        return true;
    }

    frame->type = ESP_NOW_FRAME_TYPE_UNKNOWN;
    frame->sequence = 0;
    return true;
}

static void esp_now_log_frame_details(const esp_now_frame_t *frame) {
    switch (frame->type) {
    case ESP_NOW_FRAME_TYPE_WIZMOTE:
        ESP_LOGI(TAG, "WiZmote(" MACSTR "): button=%s (%d), battery=%d%%, seq=%u",
                 MAC2STR(frame->src_mac), wizmote_button_name(frame->parsed.wizmote.button_id),
                 frame->parsed.wizmote.button_id, frame->parsed.wizmote.battery_level,
                 frame->sequence);
        break;
    default:
        ESP_LOGI(TAG, "Unknown(" MACSTR "): type=0x%02x, len=%d, seq=%d", MAC2STR(frame->src_mac),
                 frame->raw_data[0], frame->raw_len, frame->sequence);
        break;
    }
}

static void esp_now_rx_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                     void *event_data) {
    if (event_base != ESP_NOW_EVENTS) {
        return;
    }

    if (event_id == ESP_NOW_EVENT_RECV_DATA) {
        esp_now_recv_event_data_t *event = (esp_now_recv_event_data_t *)event_data;

        packets_received++;

        esp_now_frame_t frame;
        if (!esp_now_parse_frame(event->data, event->data_len, event->src_mac, event->rssi,
                                 &frame)) {
            free(event->data);
            return;
        }

        // Deduplication by sequence number (WLED pattern)
        if (frame.sequence == last_sequence) {
            packets_dropped++;
            free(event->data);
            return;
        }

        last_sequence = frame.sequence;
        esp_now_log_frame_details(&frame);

        if (user_frame_callback) {
            user_frame_callback(&frame);
        }

        // CRITICAL: Free allocated data (posted by espnow_recv_cb)
        free(event->data);
    }
}

static void esp_now_rx_adapter_init(void) {

    if (initialized) {
        ESP_LOGW(TAG, "Adapter already started");
        return;
    }

    ESP_LOGI(TAG, "Initializing adapter");

    espnow_init();

    packets_received = 0;
    packets_dropped = 0;

    esp_err_t err = esp_event_handler_instance_register(ESP_NOW_EVENTS, ESP_NOW_EVENT_RECV_DATA,
                                                        esp_now_rx_event_handler, NULL,
                                                        &espnow_event_handler_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register event handler: %s", esp_err_to_name(err));
        espnow_shutdown();
        return;
    }

    initialized = true;
}

static void esp_now_rx_adapter_shutdown(void) {

    if (!initialized) {
        ESP_LOGW(TAG, "Adapter not initialized");
        return;
    }

    ESP_LOGI(TAG, "Shutting down adapter");

    if (espnow_event_handler_instance != NULL) {
        esp_event_handler_instance_unregister(ESP_NOW_EVENTS, ESP_NOW_EVENT_RECV_DATA,
                                              espnow_event_handler_instance);
        espnow_event_handler_instance = NULL;
    }

    espnow_shutdown();
    initialized = false;
}

static void tele_esp_now_rx_stats(const char *tele_id, cJSON *json_root) {
    cJSON *stats = cJSON_CreateObject();
    cJSON_AddBoolToObject(stats, "initialized", initialized);
    cJSON_AddNumberToObject(stats, "packets_received", packets_received);
    cJSON_AddNumberToObject(stats, "packets_dropped", packets_dropped);
    cJSON_AddItemToObject(json_root, tele_id, stats);
}

static const tele_entry_t esp_now_rx_telemetry[] = {{"esp_now_rx", tele_esp_now_rx_stats},
                                                    {NULL, NULL}};

supervisor_platform_adapter_t esp_now_rx_adapter = {
    .name = "esp_now_rx",
    .init = esp_now_rx_adapter_init,
    .shutdown = esp_now_rx_adapter_shutdown,
    .tele_group = esp_now_rx_telemetry,
};
