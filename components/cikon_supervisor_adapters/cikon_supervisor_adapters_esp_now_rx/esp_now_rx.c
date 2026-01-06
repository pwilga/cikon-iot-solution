#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <string.h>

#include "cikon_esp_now.h"
#include "cmnd.h"
#include "esp_now_rx_adapter.h"
#include "json_parser.h"
#include "supervisor.h"
#include "tele.h"

#define TAG "cikon:adapter:esp_now_rx"

typedef struct {
    uint8_t program;       // 0x91 for ON button, 0x81 for all others
    uint8_t seq[4];        // 32-bit sequence number (LSB first)
    uint8_t dt1;           // Button data type (0x32)
    uint8_t button;        // Button code (1-255)
    uint8_t dt2;           // Battery data type (0x01)
    uint8_t battery_level; // Battery level (0-100)
    uint8_t checksum[4];   // Checksums (4 bytes)
} wizmote_message_t;       // Total: 13 bytes

typedef enum {
    WIZMOTE_BUTTON_ON = 1,
    WIZMOTE_BUTTON_OFF = 2,
    WIZMOTE_BUTTON_NIGHT = 3,
    WIZMOTE_BUTTON_BRIGHT_DOWN = 8,
    WIZMOTE_BUTTON_BRIGHT_UP = 9,
    WIZMOTE_BUTTON_ONE = 16,
    WIZMOTE_BUTTON_TWO = 17,
    WIZMOTE_BUTTON_THREE = 18,
    WIZMOTE_BUTTON_FOUR = 19,
} wizmote_button_t;

static const char *wizmote_button_name(uint8_t button_id) {
    switch (button_id) {
    case WIZMOTE_BUTTON_ON:
        return "WIZMOTE_BUTTON_ON";
    case WIZMOTE_BUTTON_OFF:
        return "WIZMOTE_BUTTON_OFF";
    case WIZMOTE_BUTTON_NIGHT:
        return "WIZMOTE_BUTTON_NIGHT";
    case WIZMOTE_BUTTON_BRIGHT_DOWN:
        return "WIZMOTE_BUTTON_BRIGHT_DOWN";
    case WIZMOTE_BUTTON_BRIGHT_UP:
        return "WIZMOTE_BUTTON_BRIGHT_UP";
    case WIZMOTE_BUTTON_ONE:
        return "WIZMOTE_BUTTON_ONE";
    case WIZMOTE_BUTTON_TWO:
        return "WIZMOTE_BUTTON_TWO";
    case WIZMOTE_BUTTON_THREE:
        return "WIZMOTE_BUTTON_THREE";
    case WIZMOTE_BUTTON_FOUR:
        return "WIZMOTE_BUTTON_FOUR";
    default:
        return "WIZMOTE_BUTTON_UNKNOWN";
    }
}

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

static void esp_now_rx_start(void) {
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

static void esp_now_rx_stop(void) {
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

static void cmnd_esp_now_rx(const char *args_json_str) {

    if (STATE_ON == json_str_as_logic_state(args_json_str)) {
        esp_now_rx_start();
    } else {
        esp_now_rx_stop();
    }
}

static void tele_esp_now_rx_stats(const char *tele_id, cJSON *json_root) {
    cJSON *stats = cJSON_CreateObject();
    cJSON_AddBoolToObject(stats, "initialized", initialized);
    cJSON_AddNumberToObject(stats, "packets_received", packets_received);
    cJSON_AddNumberToObject(stats, "packets_dropped", packets_dropped);
    cJSON_AddItemToObject(json_root, tele_id, stats);
}

static void esp_now_rx_adapter_init(void) {
    ESP_LOGI(TAG, "Initializing ESP-NOW RX adapter");

    packets_received = 0;
    packets_dropped = 0;
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

static const command_entry_t esp_now_rx_commands[] = {
    {"esp_now_rx", "Enable/disable ESP-NOW RX (true/false)", cmnd_esp_now_rx}, {NULL, NULL, NULL}};

static const tele_entry_t esp_now_rx_telemetry[] = {{"enow_stat", tele_esp_now_rx_stats},
                                                    {NULL, NULL}};

supervisor_platform_adapter_t esp_now_rx_adapter = {
    .init = esp_now_rx_adapter_init,
    .shutdown = esp_now_rx_adapter_shutdown,
    .on_event = NULL,
    .on_interval = NULL,
    .tele_group = esp_now_rx_telemetry,
    .cmnd_group = esp_now_rx_commands,
};
