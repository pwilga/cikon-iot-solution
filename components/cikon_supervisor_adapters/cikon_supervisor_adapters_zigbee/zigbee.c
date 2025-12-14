#include "esp_log.h"

#include "supervisor.h"
#include "zigbee_adapter.h"

#define TAG "cikon:adapter:zigbee"

// Platform-specific event bits (for future use)
#define ZIGBEE_EVENT_NETWORK_FORMED BIT2
#define ZIGBEE_EVENT_DEVICE_JOINED BIT3

// Forward declarations
static void zigbee_adapter_init(void);
static void zigbee_adapter_shutdown(void);
static void zigbee_adapter_on_event(EventBits_t bits);
static void zigbee_adapter_on_interval(supervisor_interval_stage_t stage);

// Adapter instance
supervisor_platform_adapter_t zigbee_adapter = {.init = zigbee_adapter_init,
                                                .shutdown = zigbee_adapter_shutdown,
                                                .on_event = zigbee_adapter_on_event,
                                                .on_interval = zigbee_adapter_on_interval};

// Adapter implementation (STUB/MOCK)

static void zigbee_adapter_init(void) {
    ESP_LOGI(TAG, "Initializing Zigbee platform adapter (MOCK)");

    // TODO: Initialize Zigbee stack
    // - esp_zb_platform_config()
    // - esp_zb_init()
    // - esp_zb_set_primary_network_channel_set()
    // - Configure coordinator/router role
    // - Start Zigbee network formation

    ESP_LOGW(TAG, "Zigbee adapter is currently a STUB - no real implementation");
}

static void zigbee_adapter_shutdown(void) {
    ESP_LOGI(TAG, "Shutting down Zigbee platform adapter (MOCK)");

    // TODO: Shutdown Zigbee stack
    // - Leave network
    // - Deinitialize radio
}

static void zigbee_adapter_on_event(EventBits_t bits) {
    if (bits & ZIGBEE_EVENT_NETWORK_FORMED) {
        ESP_LOGI(TAG, "Zigbee network formed (MOCK)");
    }

    if (bits & ZIGBEE_EVENT_DEVICE_JOINED) {
        ESP_LOGI(TAG, "Device joined Zigbee network (MOCK)");
    }

    // Ignore events from other adapters (INET_EVENT_*, BLE_EVENT_*, etc.)
}

static void zigbee_adapter_on_interval(supervisor_interval_stage_t stage) {
    switch (stage) {
    case SUPERVISOR_INTERVAL_1S:
        // Every second - could check network health
        break;

    case SUPERVISOR_INTERVAL_5S:
        // Every 5 seconds
        break;

    case SUPERVISOR_INTERVAL_60S:
        // Every minute - could report Zigbee network stats
        ESP_LOGD(TAG, "Zigbee periodic check (MOCK)");
        break;

    case SUPERVISOR_INTERVAL_5M:
        // Every 5 minutes
        break;

    case SUPERVISOR_INTERVAL_10M:
        // Every 10 minutes
        break;

    case SUPERVISOR_INTERVAL_2H:
        // Every 2 hours
        break;

    case SUPERVISOR_INTERVAL_12H:
        // Every 12 hours
        break;

    default:
        break;
    }
}
