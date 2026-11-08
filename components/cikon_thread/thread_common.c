#include "thread_common.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_dns64.h"
#include "esp_openthread_lock.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip6_addr.h"
#include "openthread/dataset.h"
#include "openthread/srp_client.h"
#include "openthread/thread.h"
#include "thread_commissioner.h" // IWYU pragma: keep
#include <string.h>

#define TAG "cikon:thread:common"

bool thread_dataset_parse_hex(const char *hex, otOperationalDatasetTlvs *out) {
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
        out->mTlvs[i / 2] = (uint8_t)val;
    }
    out->mLength = (uint8_t)(hex_len / 2);
    return true;
}

#if CONFIG_OPENTHREAD_SRP_CLIENT
static otSrpClientService s_srp_service;

static void srp_client_callback(otError error, const otSrpClientHostInfo *hostInfo,
                                const otSrpClientService *services,
                                const otSrpClientService *removedServices, void *context) {
    (void)hostInfo;
    (void)services;
    (void)removedServices;
    if (error == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "SRP: registered OK");
    } else if (error == OT_ERROR_DUPLICATED) {
        // SRP key mismatch — NVS was likely erased. Remove the stale host entry on the OBR.
        // To clear all SRP registrations on OBR: ot srp server disable && ot srp server enable
        ESP_LOGE(TAG, "SRP: name conflict for %s — remove stale host on OBR",
                 (const char *)context);
    } else {
        ESP_LOGW(TAG, "SRP error: %s", otThreadErrorToString(error));
    }
}

void thread_srp_host_init(const char *hostname) {
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    otSrpClientSetCallback(instance, srp_client_callback, (void *)hostname);
    otError err = otSrpClientSetHostName(instance, hostname);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "SRP set hostname: %s", otThreadErrorToString(err));
        esp_openthread_lock_release();
        return;
    }
    err = otSrpClientEnableAutoHostAddress(instance);
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "SRP auto host addr: %s", otThreadErrorToString(err));
    }
    otSrpClientEnableAutoStartMode(instance, NULL, NULL);
    ESP_LOGI(TAG, "SRP host: %s, lease: %lus", hostname,
             (unsigned long)otSrpClientGetLeaseInterval(instance));
    esp_openthread_lock_release();
}

void thread_srp_add_service(const char *instance, const char *type, uint16_t port) {
    otSrpClientService *svc = &s_srp_service;
    memset(svc, 0, sizeof(*svc));
    svc->mName = type;
    svc->mInstanceName = instance;
    svc->mPort = port;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = otSrpClientAddService(esp_openthread_get_instance(), svc);
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
        ESP_LOGE(TAG, "SRP add %s: %s", type, otThreadErrorToString(err));
    } else {
        ESP_LOGI(TAG, "SRP: %s port %d", type, port);
    }
}
#endif

#if CONFIG_OPENTHREAD_DNS64_CLIENT
bool thread_nat64_address(const char *ipv4, char *buf, size_t buflen) {
    ip6_addr_t nat64;
    if (esp_openthread_get_nat64_prefix(&nat64) != ESP_OK) {
        return false;
    }
    nat64.addr[3] = ipaddr_addr(ipv4);
    return ip6addr_ntoa_r(&nat64, buf, buflen) != NULL;
}
#endif

const command_entry_t thread_commands[] = {
#if CONFIG_OPENTHREAD_COMMISSIONER
    {"commissioner", "Let joiners that know the PSKd join the Thread network (on/off)",
     thread_commissioner_handler},
#endif
    {NULL, NULL, NULL}};
