#include "thread_scan.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/error.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/platform/radio.h" // OT_PANID_BROADCAST
#include "openthread/thread.h"

#define TAG "cikon:thread:scan"

// One network, however many of its devices answered
typedef struct {
    char name[OT_NETWORK_NAME_MAX_SIZE + 1];
    otExtendedPanId ext_panid;
    otPanId panid;
    uint8_t channel;
    int8_t rssi;     // of the best answer
    uint8_t answers; // devices of this network that answered
} thread_scan_network_t;

enum {
    SCAN_IDLE,
    SCAN_RUNNING,
    SCAN_DONE,
    SCAN_BUSY,
    SCAN_FAILED,
};
static const char *const scan_state_names[] = {
    [SCAN_IDLE] = NULL,   [SCAN_RUNNING] = "scanning", [SCAN_DONE] = "done",
    [SCAN_BUSY] = "busy", [SCAN_FAILED] = "failed",
};

// The result of the last scan, kept until the next one. The OpenThread task writes it, the tele
// reads it without a lock: a reading in the middle of a scan may show an unfinished entry, the
// next one is whole
static thread_scan_network_t s_networks[CONFIG_THREAD_SCAN_MAX];
static volatile uint8_t s_networks_count;
static volatile uint8_t s_scan_state = SCAN_IDLE;

// Called by OpenThread, in its task that already holds the lock, with the answer of one device that
// heard the scan, and with NULL when the scan is over. Every device of a network answers on its
// own, the network is told by its extended PAN ID
static void thread_scan_result_cb(otActiveScanResult *result, void *context) {
    if (result == NULL) {
        s_scan_state = SCAN_DONE;
        ESP_LOGI(TAG, "Scan done, %u network(s) heard", (unsigned)s_networks_count);
        for (uint8_t i = 0; i < s_networks_count; i++) {
            ESP_LOGI(TAG, "%s | ch %u | pan 0x%04x | rssi %d | answers %u", s_networks[i].name,
                     s_networks[i].channel, s_networks[i].panid, s_networks[i].rssi,
                     s_networks[i].answers);
        }
        return;
    }

    thread_scan_network_t *network = NULL;
    for (uint8_t i = 0; i < s_networks_count; i++) {
        if (memcmp(s_networks[i].ext_panid.m8, result->mExtendedPanId.m8,
                   sizeof(result->mExtendedPanId.m8)) == 0) {
            network = &s_networks[i];
            break;
        }
    }

    if (network) {
        if (network->answers < UINT8_MAX) {
            network->answers++;
        }
        if (result->mRssi > network->rssi) {
            network->rssi = result->mRssi;
        }
    } else if (s_networks_count < CONFIG_THREAD_SCAN_MAX) {
        network = &s_networks[s_networks_count];
        strlcpy(network->name, result->mNetworkName.m8, sizeof(network->name));
        network->ext_panid = result->mExtendedPanId;
        network->panid = result->mPanId;
        network->channel = result->mChannel;
        network->rssi = result->mRssi;
        network->answers = 1;
        s_networks_count++;
    }
}

void thread_scan_handler(const char *args_json_str) {
    (void)args_json_str;

    if (s_scan_state == SCAN_RUNNING) {
        ESP_LOGI(TAG, "Scan: already running");
        return;
    }

    s_networks_count = 0;
    s_scan_state = SCAN_RUNNING;

    otError err = OT_ERROR_NONE;
    uint8_t state = SCAN_RUNNING;

    // The lock is not ready until the OpenThread stack has started
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        otInstance *instance = esp_openthread_get_instance();

        // The discovery goes over the IPv6 interface, a device without a network may have it down
        if (!otIp6IsEnabled(instance)) {
            err = otIp6SetEnabled(instance, true);
        }
        if (err == OT_ERROR_NONE) {
            // 0: all the channels
            err = otThreadDiscover(instance, 0, OT_PANID_BROADCAST, false, false,
                                   thread_scan_result_cb, NULL);
        }
        esp_openthread_lock_release();

        // Busy: the joiner is on an attempt, which takes seconds, or another scan is on
        if (err != OT_ERROR_NONE) {
            state = err == OT_ERROR_BUSY ? SCAN_BUSY : SCAN_FAILED;
        }
    } else {
        state = SCAN_FAILED;
    }

    if (state == SCAN_RUNNING) {
        ESP_LOGI(TAG, "Scanning for Thread networks");
        return;
    }

    s_scan_state = state;
    ESP_LOGW(TAG, "Scan %s: %s", scan_state_names[state], otThreadErrorToString(err));
}

void thread_scan_tele(cJSON *thread) {
    uint8_t state = s_scan_state;
    if (state == SCAN_IDLE) {
        return;
    }

    cJSON *scan = cJSON_AddObjectToObject(thread, "scan");
    cJSON_AddStringToObject(scan, "state", scan_state_names[state]);

    cJSON *networks = cJSON_AddArrayToObject(scan, "networks");
    uint8_t count = s_networks_count;
    for (uint8_t i = 0; i < count; i++) {
        const thread_scan_network_t *network = &s_networks[i];
        cJSON *item = cJSON_CreateObject();

        char ext_panid[2 * sizeof(network->ext_panid.m8) + 1];
        for (size_t j = 0; j < sizeof(network->ext_panid.m8); j++) {
            snprintf(&ext_panid[2 * j], 3, "%02x", network->ext_panid.m8[j]);
        }
        char panid[7];
        snprintf(panid, sizeof(panid), "0x%04x", network->panid);

        cJSON_AddStringToObject(item, "name", network->name);
        cJSON_AddStringToObject(item, "ext_panid", ext_panid);
        cJSON_AddStringToObject(item, "panid", panid);
        cJSON_AddNumberToObject(item, "channel", network->channel);
        cJSON_AddNumberToObject(item, "rssi", network->rssi);
        cJSON_AddNumberToObject(item, "answers", network->answers);
        cJSON_AddItemToArray(networks, item);
    }
}
