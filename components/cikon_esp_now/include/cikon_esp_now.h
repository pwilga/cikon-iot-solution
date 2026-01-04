#pragma once

#include "esp_err.h"
#include "esp_event_base.h"
#include <stdint.h>

ESP_EVENT_DECLARE_BASE(ESP_NOW_EVENTS);

typedef enum {
    ESP_NOW_EVENT_SEND_DONE,    // Packet send complete
    ESP_NOW_EVENT_RECV_DATA,    // Packet received
    ESP_NOW_EVENT_PEER_ADDED,   // Peer added
    ESP_NOW_EVENT_PEER_REMOVED, // Peer removed
} espnow_event_id_t;

typedef struct {
    uint8_t mac_addr[6];
    bool success; // Send status
} esp_now_send_event_data_t;

typedef struct {
    uint8_t src_mac[6];
    uint8_t *data;
    size_t data_len;
    int8_t rssi;
} esp_now_recv_event_data_t;

esp_err_t espnow_init(void);
esp_err_t espnow_shutdown(void);
