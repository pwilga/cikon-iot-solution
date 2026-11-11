#include <stdint.h>
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
#include "openthread/message.h" // IWYU pragma: keep
#include "openthread/platform/radio.h"
#include "openthread/srp_server.h" // IWYU pragma: keep
#include "openthread/thread.h"

#include "thread_commissioner.h" // IWYU pragma: keep
#include "thread_tele.h"

#define TAG "cikon:thread:tele"

#define THREAD_TELE_MAX_ADDRESSES 8
#define THREAD_TELE_MAX_DEVICES 16
#define THREAD_TELE_MAX_NEIGHBORS 8

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

#if CONFIG_THREAD_TELE_COUNTERS
    // Counters since the start of the device, copied as they are
    struct {
        uint32_t tx_total;
        uint32_t tx_retry;
        uint32_t tx_err_cca;
        uint32_t tx_err_busy;
        uint32_t tx_max_retry;
        uint32_t rx_total;
        uint32_t rx_err_fcs;
        uint16_t cca_fail; // of the latest frames, 0xffff is 100%
    } mac;
    struct {
        uint16_t detached;
        uint16_t child;
        uint16_t router;
        uint16_t leader;
        uint16_t attach_attempts;
        uint16_t partition_changes;
        uint16_t parent_changes;
    } mle;
    struct {
        uint32_t tx_ok;
        uint32_t tx_fail;
        uint32_t rx_ok;
        uint32_t rx_fail;
    } ip;
    struct {
        uint16_t total;
        uint16_t free;
        uint16_t max_used; // the most buffers taken at once since the start
        uint16_t send;     // taken by the 6LoWPAN send queue
        uint16_t reassembly;
    } buffers;
    struct {
        uint16_t rloc16;
        int8_t rssi;
        int8_t last_rssi;
        uint8_t lq;
        uint32_t age; // seconds since it was last heard
        bool child;
    } neighbors[THREAD_TELE_MAX_NEIGHBORS];
    uint8_t neighbors_num;
#endif

#if CONFIG_OPENTHREAD_BORDER_ROUTER
#if CONFIG_THREAD_TELE_COUNTERS
    struct {
        otBorderRoutingState state;
        otSrpServerState srp;
        uint64_t in_pkts; // unicast, Thread to the LAN and back
        uint64_t in_bytes;
        uint64_t out_pkts;
        uint64_t out_bytes;
        uint32_t ra_rx;
        uint32_t ra_tx;
        uint32_t ra_tx_fail;
        uint32_t rs_rx;
        uint32_t rs_tx;
        uint32_t rs_tx_fail;
        uint8_t routers; // other routers on the LAN
    } br;
#endif
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

#if CONFIG_THREAD_TELE_COUNTERS
    const otMacCounters *mac = otLinkGetCounters(instance);
    s_data.mac.tx_total = mac->mTxTotal;
    s_data.mac.tx_retry = mac->mTxRetry;
    s_data.mac.tx_err_cca = mac->mTxErrCca;
    s_data.mac.tx_err_busy = mac->mTxErrBusyChannel;
    s_data.mac.tx_max_retry = mac->mTxDirectMaxRetryExpiry;
    s_data.mac.rx_total = mac->mRxTotal;
    s_data.mac.rx_err_fcs = mac->mRxErrFcs;
    s_data.mac.cca_fail = otLinkGetCcaFailureRate(instance);

    const otMleCounters *mle = otThreadGetMleCounters(instance);
    s_data.mle.detached = mle->mDetachedRole;
    s_data.mle.child = mle->mChildRole;
    s_data.mle.router = mle->mRouterRole;
    s_data.mle.leader = mle->mLeaderRole;
    s_data.mle.attach_attempts = mle->mAttachAttempts;
    s_data.mle.partition_changes = mle->mPartitionIdChanges;
    s_data.mle.parent_changes = mle->mParentChanges;

    const otIpCounters *ip = otThreadGetIp6Counters(instance);
    s_data.ip.tx_ok = ip->mTxSuccess;
    s_data.ip.tx_fail = ip->mTxFailure;
    s_data.ip.rx_ok = ip->mRxSuccess;
    s_data.ip.rx_fail = ip->mRxFailure;

    otBufferInfo buffer_info;
    otMessageGetBufferInfo(instance, &buffer_info);
    s_data.buffers.total = buffer_info.mTotalBuffers;
    s_data.buffers.free = buffer_info.mFreeBuffers;
    s_data.buffers.max_used = buffer_info.mMaxUsedBuffers;
    s_data.buffers.send = buffer_info.m6loSendQueue.mNumBuffers;
    s_data.buffers.reassembly = buffer_info.m6loReassemblyQueue.mNumBuffers;

    s_data.neighbors_num = 0;
    otNeighborInfoIterator neighbor_it = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    otNeighborInfo neighbor;
    while (s_data.neighbors_num < THREAD_TELE_MAX_NEIGHBORS &&
           otThreadGetNextNeighborInfo(instance, &neighbor_it, &neighbor) == OT_ERROR_NONE) {
        uint8_t i = s_data.neighbors_num++;
        s_data.neighbors[i].rloc16 = neighbor.mRloc16;
        s_data.neighbors[i].rssi = neighbor.mAverageRssi;
        s_data.neighbors[i].last_rssi = neighbor.mLastRssi;
        s_data.neighbors[i].lq = neighbor.mLinkQualityIn;
        s_data.neighbors[i].age = neighbor.mAge;
        s_data.neighbors[i].child = neighbor.mIsChild;
    }
#endif

#if CONFIG_OPENTHREAD_BORDER_ROUTER
#if CONFIG_THREAD_TELE_COUNTERS
    const otBorderRoutingCounters *br = otIp6GetBorderRoutingCounters(instance);
    s_data.br.in_pkts = br->mInboundUnicast.mPackets;
    s_data.br.in_bytes = br->mInboundUnicast.mBytes;
    s_data.br.out_pkts = br->mOutboundUnicast.mPackets;
    s_data.br.out_bytes = br->mOutboundUnicast.mBytes;
    s_data.br.ra_rx = br->mRaRx;
    s_data.br.ra_tx = br->mRaTxSuccess;
    s_data.br.ra_tx_fail = br->mRaTxFailure;
    s_data.br.rs_rx = br->mRsRx;
    s_data.br.rs_tx = br->mRsTxSuccess;
    s_data.br.rs_tx_fail = br->mRsTxFailure;
    s_data.br.state = otBorderRoutingGetState(instance);
    s_data.br.srp = otSrpServerGetState(instance);

    s_data.br.routers = 0;
    otBorderRoutingPrefixTableIterator router_it;
    otBorderRoutingRouterEntry router;
    otBorderRoutingPrefixTableInitIterator(instance, &router_it);
    while (otBorderRoutingGetNextRouterEntry(instance, &router_it, &router) == OT_ERROR_NONE) {
        if (!router.mIsLocalDevice && s_data.br.routers < UINT8_MAX) {
            s_data.br.routers++;
        }
    }
#endif

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

#if CONFIG_THREAD_TELE_COUNTERS
#if CONFIG_OPENTHREAD_BORDER_ROUTER
static const char *thread_tele_routing_state(otBorderRoutingState state) {
    switch (state) {
    case OT_BORDER_ROUTING_STATE_UNINITIALIZED:
        return "uninitialized";
    case OT_BORDER_ROUTING_STATE_DISABLED:
        return "disabled";
    case OT_BORDER_ROUTING_STATE_STOPPED:
        return "stopped";
    case OT_BORDER_ROUTING_STATE_RUNNING:
        return "running";
    }
    return "unknown";
}

static const char *thread_tele_srp_state(otSrpServerState state) {
    switch (state) {
    case OT_SRP_SERVER_STATE_DISABLED:
        return "disabled";
    case OT_SRP_SERVER_STATE_RUNNING:
        return "running";
    case OT_SRP_SERVER_STATE_STOPPED:
        return "stopped";
    }
    return "unknown";
}
#endif
#endif // CONFIG_THREAD_TELE_COUNTERS

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

#if CONFIG_THREAD_TELE_COUNTERS
    // Counters since the start of the device: the ratio of two of them is the lifetime average
    cJSON *mac = cJSON_AddObjectToObject(thread, "mac");
    cJSON_AddNumberToObject(mac, "tx_total", s_data.mac.tx_total);
    cJSON_AddNumberToObject(mac, "tx_retry", s_data.mac.tx_retry);
    cJSON_AddNumberToObject(mac, "tx_err_cca", s_data.mac.tx_err_cca);
    cJSON_AddNumberToObject(mac, "tx_err_busy", s_data.mac.tx_err_busy);
    cJSON_AddNumberToObject(mac, "tx_max_retry", s_data.mac.tx_max_retry);
    cJSON_AddNumberToObject(mac, "rx_total", s_data.mac.rx_total);
    cJSON_AddNumberToObject(mac, "rx_err_fcs", s_data.mac.rx_err_fcs);
    // Of the latest frames, not since the start
    cJSON_AddNumberToObject(mac, "cca_fail_pct", s_data.mac.cca_fail * 100.0 / UINT16_MAX);

    cJSON *mle = cJSON_AddObjectToObject(thread, "mle");
    cJSON_AddNumberToObject(mle, "detached", s_data.mle.detached);
    cJSON_AddNumberToObject(mle, "child", s_data.mle.child);
    cJSON_AddNumberToObject(mle, "router", s_data.mle.router);
    cJSON_AddNumberToObject(mle, "leader", s_data.mle.leader);
    cJSON_AddNumberToObject(mle, "attach_attempts", s_data.mle.attach_attempts);
    cJSON_AddNumberToObject(mle, "partition_changes", s_data.mle.partition_changes);
    cJSON_AddNumberToObject(mle, "parent_changes", s_data.mle.parent_changes);

    cJSON *ip = cJSON_AddObjectToObject(thread, "ip");
    cJSON_AddNumberToObject(ip, "tx_ok", s_data.ip.tx_ok);
    cJSON_AddNumberToObject(ip, "tx_fail", s_data.ip.tx_fail);
    cJSON_AddNumberToObject(ip, "rx_ok", s_data.ip.rx_ok);
    cJSON_AddNumberToObject(ip, "rx_fail", s_data.ip.rx_fail);

    cJSON *buffers = cJSON_AddObjectToObject(thread, "buffers");
    cJSON_AddNumberToObject(buffers, "total", s_data.buffers.total);
    cJSON_AddNumberToObject(buffers, "free", s_data.buffers.free);
    cJSON_AddNumberToObject(buffers, "max_used", s_data.buffers.max_used);
    cJSON_AddNumberToObject(buffers, "send", s_data.buffers.send);
    cJSON_AddNumberToObject(buffers, "reassembly", s_data.buffers.reassembly);

    cJSON *neighbors = cJSON_AddArrayToObject(thread, "neighbors");
    for (uint8_t i = 0; i < s_data.neighbors_num; i++) {
        cJSON *neighbor = cJSON_CreateObject();
        cJSON_AddNumberToObject(neighbor, "rloc16", s_data.neighbors[i].rloc16);
        cJSON_AddNumberToObject(neighbor, "rssi", s_data.neighbors[i].rssi);
        cJSON_AddNumberToObject(neighbor, "last_rssi", s_data.neighbors[i].last_rssi);
        cJSON_AddNumberToObject(neighbor, "lq", s_data.neighbors[i].lq);
        cJSON_AddNumberToObject(neighbor, "age", s_data.neighbors[i].age);
        cJSON_AddBoolToObject(neighbor, "child", s_data.neighbors[i].child);
        cJSON_AddItemToArray(neighbors, neighbor);
    }

#if CONFIG_OPENTHREAD_BORDER_ROUTER
    cJSON *br = cJSON_AddObjectToObject(thread, "br");
    cJSON_AddStringToObject(br, "state", thread_tele_routing_state(s_data.br.state));
    cJSON_AddStringToObject(br, "srp", thread_tele_srp_state(s_data.br.srp));
    cJSON_AddNumberToObject(br, "in_pkts", s_data.br.in_pkts);
    cJSON_AddNumberToObject(br, "in_bytes", s_data.br.in_bytes);
    cJSON_AddNumberToObject(br, "out_pkts", s_data.br.out_pkts);
    cJSON_AddNumberToObject(br, "out_bytes", s_data.br.out_bytes);
    cJSON_AddNumberToObject(br, "ra_rx", s_data.br.ra_rx);
    cJSON_AddNumberToObject(br, "ra_tx", s_data.br.ra_tx);
    cJSON_AddNumberToObject(br, "ra_tx_fail", s_data.br.ra_tx_fail);
    cJSON_AddNumberToObject(br, "rs_rx", s_data.br.rs_rx);
    cJSON_AddNumberToObject(br, "rs_tx", s_data.br.rs_tx);
    cJSON_AddNumberToObject(br, "rs_tx_fail", s_data.br.rs_tx_fail);
    cJSON_AddNumberToObject(br, "routers", s_data.br.routers);
#endif
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
