#pragma once

#include "supervisor.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t button_id;
    uint8_t battery_level;
} esp_now_wizmote_data_t;

typedef enum {
    ESP_NOW_FRAME_TYPE_WIZMOTE = 0x91,
    ESP_NOW_FRAME_TYPE_CIKON = 0x01,
    ESP_NOW_FRAME_TYPE_UNKNOWN = 0xFF
} esp_now_frame_type_t;

typedef struct {
    esp_now_frame_type_t type;
    uint8_t sequence;
    uint8_t src_mac[6];
    int8_t rssi;

    union {
        esp_now_wizmote_data_t wizmote;
    } parsed;

    const uint8_t *raw_data;
    size_t raw_len;
} esp_now_frame_t;

typedef void (*esp_now_frame_callback_t)(const esp_now_frame_t *frame);

void esp_now_rx_register_callback(esp_now_frame_callback_t callback);

extern supervisor_platform_adapter_t esp_now_rx_adapter;
