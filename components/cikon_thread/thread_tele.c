#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/border_agent.h"
#include "openthread/border_routing.h" // IWYU pragma: keep
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/platform/radio.h"
#include "openthread/srp_server.h" // IWYU pragma: keep
#include "openthread/thread.h"

#include "thread_commissioner.h" // IWYU pragma: keep
#include "thread_tele.h"

#define TAG "cikon:thread:tele"

#define THREAD_TELE_MAX_ADDRESSES 8
#define THREAD_TELE_MAX_DEVICES 16

// The Thread network state, copied from OpenThread so that the services reading it never take the
// OpenThread lock
static struct {
    otDeviceRole role;
    char network[OT_NETWORK_NAME_MAX_SIZE + 1];
    uint8_t channel;
    int8_t tx_power; // read once: asking the RCP goes over UART, and nothing changes it later
    otPanId panid;
    otExtendedPanId ext_panid;
    uint32_t partition_id;
    otExtAddress eui64;
#if CONFIG_OPENTHREAD_BORDER_AGENT_ENABLE
    otBorderAgentId ba_id;
    bool has_ba_id;
#endif
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    otIp6Prefix omr; // length 0 when there is none
#endif
    otIp6Address addresses[THREAD_TELE_MAX_ADDRESSES];
    const char *address_types[THREAD_TELE_MAX_ADDRESSES]; // NULL when none fits
    uint8_t addresses_num;
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    struct {
        char name[32];
        otIp6Address ip; // unspecified when the device registered none
    } devices[THREAD_TELE_MAX_DEVICES];
    uint8_t devices_num;
#endif
} s_data = {.tx_power = OT_RADIO_POWER_INVALID};

static bool s_data_valid = false;
static SemaphoreHandle_t s_data_mutex = NULL;

// What the address is for - OpenThread only tells where it came from ("thread" for most of them)
static const char *thread_tele_address_type(const otNetifAddress *addr) {
    const uint8_t *bytes = addr->mAddress.mFields.m8;

    // fe80::/10
    if (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80) {
        return "link-local";
    }

    // RLOC and ALOC end with 0000:00ff:fe00:<locator>, anycast locators from 0xfc00 up
    static const uint8_t locator_iid[] = {0x00, 0x00, 0x00, 0xff, 0xfe, 0x00};
    if (memcmp(&bytes[8], locator_iid, sizeof(locator_iid)) == 0) {
        return bytes[14] >= 0xfc ? "aloc" : "rloc";
    }

    if (addr->mMeshLocal) {
        return "ml-eid";
    }

    if (addr->mAddressOrigin == OT_ADDRESS_ORIGIN_SLAAC) {
        return "omr";
    }

    return NULL;
}

// Only copying under the OpenThread lock - formatting happens without it
void thread_tele_refresh(void) {
    // Called from one task only, so the mutex is created here without a race - the tele sees NULL
    // until then
    if (s_data_mutex == NULL) {
        s_data_mutex = xSemaphoreCreateMutex();
        if (s_data_mutex == NULL) {
            return;
        }
    }

    // Other tasks read the data - never while it's half written. The mutex comes first so the data
    // is filled in place, without a second buffer
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);

    // The lock is not ready until the OpenThread stack has started. A busy one skips this refresh
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
        xSemaphoreGive(s_data_mutex);
        return;
    }

    otInstance *instance = esp_openthread_get_instance();

    s_data.role = otThreadGetDeviceRole(instance);
    strlcpy(s_data.network, otThreadGetNetworkName(instance), sizeof(s_data.network));
    s_data.channel = otLinkGetChannel(instance);
    s_data.panid = otLinkGetPanId(instance);
    s_data.ext_panid = *otThreadGetExtendedPanId(instance);
    s_data.partition_id = otThreadGetPartitionId(instance);
    otLinkGetFactoryAssignedIeeeEui64(instance, &s_data.eui64);

    if (s_data.tx_power == OT_RADIO_POWER_INVALID &&
        otPlatRadioGetTransmitPower(instance, &s_data.tx_power) != OT_ERROR_NONE) {
        s_data.tx_power = OT_RADIO_POWER_INVALID;
    }

#if CONFIG_OPENTHREAD_BORDER_AGENT_ENABLE
    s_data.has_ba_id = otBorderAgentGetId(instance, &s_data.ba_id) == OT_ERROR_NONE;
#endif
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    if (otBorderRoutingGetOmrPrefix(instance, &s_data.omr) != OT_ERROR_NONE) {
        s_data.omr.mLength = 0;
    }
#endif

    s_data.addresses_num = 0;
    for (const otNetifAddress *addr = otIp6GetUnicastAddresses(instance);
         addr && s_data.addresses_num < THREAD_TELE_MAX_ADDRESSES; addr = addr->mNext) {
        s_data.addresses[s_data.addresses_num] = addr->mAddress;
        s_data.address_types[s_data.addresses_num] = thread_tele_address_type(addr);
        s_data.addresses_num++;
    }

#if CONFIG_OPENTHREAD_BORDER_ROUTER
    // Devices registered on the BR's SRP server, by the name they announce themselves with
    s_data.devices_num = 0;
    const otSrpServerHost *host = NULL;
    while ((host = otSrpServerGetNextHost(instance, host)) != NULL &&
           s_data.devices_num < THREAD_TELE_MAX_DEVICES) {
        if (otSrpServerHostIsDeleted(host)) {
            continue;
        }

        uint8_t i = s_data.devices_num++;
        memset(&s_data.devices[i], 0, sizeof(s_data.devices[i]));

        // "h2dev.default.service.arpa." - the device name is the first label
        const char *full_name = otSrpServerHostGetFullName(host);
        snprintf(s_data.devices[i].name, sizeof(s_data.devices[i].name), "%.*s",
                 (int)strcspn(full_name, "."), full_name);

        uint8_t addresses_num = 0;
        const otIp6Address *addresses = otSrpServerHostGetAddresses(host, &addresses_num);
        if (addresses_num > 0) {
            s_data.devices[i].ip = addresses[0];
        }
    }
#endif

    esp_openthread_lock_release();

    s_data_valid = true;
    xSemaphoreGive(s_data_mutex);
}

static void thread_tele_hex_appender(cJSON *json, const char *key, const uint8_t *bytes,
                                     size_t len) {
    char hex[2 * OT_BORDER_AGENT_ID_LENGTH + 1];
    for (size_t i = 0; i < len && i < OT_BORDER_AGENT_ID_LENGTH; i++) {
        snprintf(&hex[2 * i], 3, "%02x", bytes[i]);
    }
    cJSON_AddStringToObject(json, key, hex);
}

void thread_tele_appender(const char *tele_id, cJSON *json_root) {
    if (s_data_mutex == NULL || xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    if (!s_data_valid) {
        xSemaphoreGive(s_data_mutex);
        return;
    }

    cJSON *thread = cJSON_AddObjectToObject(json_root, tele_id);
    char buf[OT_IP6_PREFIX_STRING_SIZE];

    cJSON_AddStringToObject(thread, "role", otThreadDeviceRoleToString(s_data.role));
    cJSON_AddStringToObject(thread, "network", s_data.network);
    cJSON_AddNumberToObject(thread, "channel", s_data.channel);

    if (s_data.tx_power != OT_RADIO_POWER_INVALID) {
        cJSON_AddNumberToObject(thread, "tx_power", s_data.tx_power);
    }

    snprintf(buf, sizeof(buf), "0x%04x", s_data.panid);
    cJSON_AddStringToObject(thread, "panid", buf);
    thread_tele_hex_appender(thread, "ext_panid", s_data.ext_panid.m8, sizeof(s_data.ext_panid.m8));
    cJSON_AddNumberToObject(thread, "partition_id", s_data.partition_id);
    thread_tele_hex_appender(thread, "eui64", s_data.eui64.m8, sizeof(s_data.eui64.m8));

#if CONFIG_OPENTHREAD_BORDER_AGENT_ENABLE
    if (s_data.has_ba_id) {
        thread_tele_hex_appender(thread, "ba_id", s_data.ba_id.mId, sizeof(s_data.ba_id.mId));
    }
#endif

#if CONFIG_OPENTHREAD_BORDER_ROUTER
    if (s_data.omr.mLength != 0) {
        otIp6PrefixToString(&s_data.omr, buf, sizeof(buf));
        cJSON_AddStringToObject(thread, "omr", buf);
    }
#endif

    cJSON *addresses = cJSON_AddArrayToObject(thread, "addresses");
    for (uint8_t i = 0; i < s_data.addresses_num; i++) {
        cJSON *address = cJSON_CreateObject();
        otIp6AddressToString(&s_data.addresses[i], buf, sizeof(buf));
        cJSON_AddStringToObject(address, "ip", buf);
        if (s_data.address_types[i]) {
            cJSON_AddStringToObject(address, "type", s_data.address_types[i]);
        }
        cJSON_AddItemToArray(addresses, address);
    }

#if CONFIG_OPENTHREAD_BORDER_ROUTER
    cJSON *devices = cJSON_AddArrayToObject(thread, "devices");
    for (uint8_t i = 0; i < s_data.devices_num; i++) {
        cJSON *device = cJSON_CreateObject();
        cJSON_AddStringToObject(device, "name", s_data.devices[i].name);
        if (!otIp6IsAddressUnspecified(&s_data.devices[i].ip)) {
            otIp6AddressToString(&s_data.devices[i].ip, buf, sizeof(buf));
            cJSON_AddStringToObject(device, "ip", buf);
        }
        cJSON_AddItemToArray(devices, device);
    }
#endif

    thread_commissioner_tele(thread);

    xSemaphoreGive(s_data_mutex);
}

void thread_tele_ip_appender(const char *tele_id, cJSON *json_root) {
    if (s_data_mutex == NULL || xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    // OMR is the address routed onto the LAN, the mesh-local EID is the fallback
    const otIp6Address *best = NULL;
    for (uint8_t i = 0; s_data_valid && i < s_data.addresses_num; i++) {
        const char *type = s_data.address_types[i];
        if (type && strcmp(type, "omr") == 0) {
            best = &s_data.addresses[i];
            break;
        }
        if (type && strcmp(type, "ml-eid") == 0 && !best) {
            best = &s_data.addresses[i];
        }
    }

    if (best) {
        char buf[OT_IP6_ADDRESS_STRING_SIZE];
        otIp6AddressToString(best, buf, sizeof(buf));
        cJSON_AddStringToObject(json_root, tele_id, buf);
    }

    xSemaphoreGive(s_data_mutex);
}

void thread_tele_log_network(void) {
    if (s_data_mutex == NULL || xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    if (s_data_valid) {
        // The RLOC16 is the last two bytes of the RLOC address
        uint16_t rloc16 = OT_RADIO_INVALID_SHORT_ADDR;
        for (uint8_t i = 0; i < s_data.addresses_num; i++) {
            const char *type = s_data.address_types[i];
            if (type && strcmp(type, "rloc") == 0) {
                rloc16 =
                    (s_data.addresses[i].mFields.m8[14] << 8) | s_data.addresses[i].mFields.m8[15];
                break;
            }
        }

        ESP_LOGI(TAG, "%s | %s | ch=%u | pan=0x%04X | rloc=0x%04X | part=%lu",
                 otThreadDeviceRoleToString(s_data.role), s_data.network, s_data.channel,
                 s_data.panid, rloc16, (unsigned long)s_data.partition_id);
    }

    xSemaphoreGive(s_data_mutex);
}
