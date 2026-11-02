#include "thread_border_router_adapter.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h" // IWYU pragma: keep
#include "esp_openthread_types.h"
#include "lwip/nd6.h"
#include "lwip/tcpip.h"
#include "openthread/border_agent.h"
#include "openthread/border_routing.h"
#include "openthread/dataset_ftd.h"
#include "openthread/error.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/platform/radio.h"
#include "openthread/srp_server.h"
#include "openthread/thread.h"

#include "cJSON.h"

#include "bits_helper.h"
#include "config_manager.h"
#include "supervisor.h"
#include "tele.h"
#include "thread_common.h"
#include "thread_radio_config.h"

#define TAG "cikon:adapter:thread_border_router"

static esp_event_handler_instance_t s_ip6_handler =
    NULL; // one-shot: fires once backbone IPv6 link-local is up, then unregisters itself
static esp_event_handler_instance_t s_role_handler =
    NULL; // one-shot: fires once Thread attaches as Leader/Router, then unregisters itself
static esp_event_handler_instance_t s_ip6_back_handler =
    NULL; // stays: backbone IPv6 address back after an outage
static esp_netif_t *s_backbone_netif = NULL;

static bool initialized = false;
static bool s_ot_started = false;

// Read once: asking the RCP goes over UART, and nothing changes it after the start
static int8_t s_tx_power;
static bool s_tx_power_known = false;

#define THREAD_SNAPSHOT_MAX_ADDRESSES 8
#define THREAD_SNAPSHOT_MAX_DEVICES 16

typedef struct {
    char name[32];
    otIp6Address ip;
    bool has_ip;
} thread_snapshot_device_t;

// The Thread network for the tele, copied from OpenThread every 10 s. The tele only formats it, so
// MQTT and HTTP never take the OpenThread lock
typedef struct {
    otDeviceRole role;
    char network[OT_NETWORK_NAME_MAX_SIZE + 1];
    uint8_t channel;
    otPanId panid;
    otExtendedPanId ext_panid;
    uint32_t partition_id;
    otExtAddress eui64;
    otBorderAgentId ba_id;
    bool has_ba_id;
    otIp6Prefix omr;
    bool has_omr;
    otIp6Address addresses[THREAD_SNAPSHOT_MAX_ADDRESSES];
    const char *address_types[THREAD_SNAPSHOT_MAX_ADDRESSES]; // NULL when none fits
    uint8_t addresses_num;
    thread_snapshot_device_t devices[THREAD_SNAPSHOT_MAX_DEVICES];
    uint8_t devices_num;
} thread_snapshot_t;

static thread_snapshot_t s_snapshot;
static bool s_snapshot_valid = false;
static SemaphoreHandle_t s_snapshot_mutex = NULL;

static const esp_openthread_config_t s_ot_config = THREAD_DEFAULT_OT_CONFIG();

// Thread starts without waiting for the backbone, border routing joins later
static void thread_start_task(void *arg) {

    esp_openthread_lock_acquire(portMAX_DELAY);

    otOperationalDatasetTlvs dataset;

    if (otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset) == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "Using dataset from NVS");
    } else {
        otOperationalDataset config_dataset;
        if (otDatasetCreateNewNetwork(esp_openthread_get_instance(), &config_dataset) !=
            OT_ERROR_NONE) {
            ESP_LOGE(TAG, "Failed to create new Thread dataset");
            esp_openthread_lock_release();
            goto done;
        }

        memcpy(config_dataset.mNetworkName.m8, CONFIG_THREAD_BORDER_ROUTER_NETWORK_NAME,
               strlen(CONFIG_THREAD_BORDER_ROUTER_NETWORK_NAME) + 1);
        config_dataset.mComponents.mIsNetworkNamePresent = true;

        config_dataset.mChannel = CONFIG_THREAD_BORDER_ROUTER_CHANNEL;
        config_dataset.mComponents.mIsChannelPresent = true;

#if CONFIG_THREAD_BORDER_ROUTER_WAKEUP_CHANNEL > 0
        config_dataset.mWakeupChannel = CONFIG_THREAD_BORDER_ROUTER_WAKEUP_CHANNEL;
        config_dataset.mComponents.mIsWakeupChannelPresent = true;
#endif

        otDatasetConvertToTlvs(&config_dataset, &dataset);
        ESP_LOGI(TAG, "Created new Thread dataset: %s ch%d",
                 CONFIG_THREAD_BORDER_ROUTER_NETWORK_NAME, CONFIG_THREAD_BORDER_ROUTER_CHANNEL);
    }

    esp_openthread_auto_start(&dataset);
    s_tx_power_known =
        otPlatRadioGetTransmitPower(esp_openthread_get_instance(), &s_tx_power) == OT_ERROR_NONE;
    esp_openthread_lock_release();
    ESP_LOGI(TAG, "Thread network started");

done:
    vTaskDelete(NULL);
}

static esp_err_t thread_border_router_adapter_init(void) {
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_snapshot_mutex == NULL) {
        s_snapshot_mutex = xSemaphoreCreateMutex();
    }

    // esp_netif_init() already called by the network adapter (uplink) before us
#if CONFIG_OPENTHREAD_CLI
    {
        static char repl_prompt[22]; // "(br) " (5) + dev_name (max 15) + ">" (1) + '\0' (1)
        snprintf(repl_prompt, sizeof(repl_prompt), "(br) %s>", config_get()->dev_name);
        thread_console_start(repl_prompt);
    }
#endif
    ESP_LOGI(TAG, "Starting OpenThread stack");
    esp_err_t err = esp_openthread_start(&s_ot_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_openthread_start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ot_started = true;

#if CONFIG_OPENTHREAD_CLI
    thread_cli_commands_init();
#endif

    xTaskCreate(thread_start_task, "thread_start", 6144, NULL, 5, NULL);

    ESP_LOGI(TAG, "OpenThread stack started, waiting for backbone IP");
    return ESP_OK;
}

static esp_err_t thread_border_router_adapter_shutdown(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (initialized) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        esp_openthread_border_router_deinit();
        esp_openthread_lock_release();
        initialized = false;
    }
    initialized = false;
    return ESP_OK;
}

// After an OBR restart, backbone hosts miss the OMR prefix because the first RA can be delayed by
// minutes. Cycling border routing here triggers an immediate RA so hosts get the prefix right after
// attach.
static void on_thread_role_changed(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_openthread_lock_acquire(portMAX_DELAY);
    otDeviceRole role = otThreadGetDeviceRole(esp_openthread_get_instance());
    if (role == OT_DEVICE_ROLE_LEADER || role == OT_DEVICE_ROLE_ROUTER) {
        // Unregister first — one-shot, we only need to force RA after the initial attach
        // post-restart
        esp_openthread_lock_release();
        if (s_role_handler) {
            esp_event_handler_instance_unregister(OPENTHREAD_EVENT, OPENTHREAD_EVENT_ROLE_CHANGED,
                                                  s_role_handler);
            s_role_handler = NULL;
        }

        // Re-cycle border routing to trigger an immediate RA on backbone.
        esp_openthread_lock_acquire(portMAX_DELAY);
        (void)otBorderRoutingSetEnabled(esp_openthread_get_instance(), false);
        (void)otBorderRoutingSetEnabled(esp_openthread_get_instance(), true);
        esp_openthread_lock_release();
        ESP_LOGI(TAG, "Thread %s — border routing RA refreshed on backbone",
                 otThreadDeviceRoleToString(role));
    } else {
        esp_openthread_lock_release();
    }
}

static void border_router_start_task(void *arg) {

    esp_openthread_lock_acquire(portMAX_DELAY);

    esp_openthread_set_backbone_netif(s_backbone_netif);
    esp_err_t err = esp_openthread_border_router_init();
    esp_openthread_lock_release();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Border router init failed: %s", esp_err_to_name(err));
        goto done;
    }

    esp_event_handler_instance_register(OPENTHREAD_EVENT, OPENTHREAD_EVENT_ROLE_CHANGED,
                                        on_thread_role_changed, NULL, &s_role_handler);
    // Thread may already be leader/router by now - then no role change comes, refresh right away
    on_thread_role_changed(NULL, NULL, 0, NULL);

    initialized = true;
    supervisor_notify_event(THREAD_BORDER_ROUTER_READY);
    ESP_LOGI(TAG, "Thread Border Router started");

done:
    vTaskDelete(NULL);
}

static void on_backbone_ip6(void *arg, esp_event_base_t base, int32_t id, void *data) {
    ip_event_got_ip6_t *evt = (ip_event_got_ip6_t *)data;
    if (evt->esp_netif != s_backbone_netif || initialized) {
        return;
    }
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_GOT_IP6, s_ip6_handler);
    s_ip6_handler = NULL;
    ESP_LOGI(TAG, "Backbone IPv6 link-local ready, starting Border Router");
    xTaskCreate(border_router_start_task, "br_start", 6144, NULL, 5, NULL);
}

static void clear_ip6_route_cache(void *arg) { nd6_clear_destination_cache(); }

// Global IPv6 address back on the backbone = its on-link prefix is known again. Hosts that sent
// traffic into Thread before that got cached as "via the default router", and lwIP keeps that
// after the prefix returns - their replies stay misrouted until the cache is cleared
static void on_backbone_ip6_back(void *arg, esp_event_base_t base, int32_t id, void *data) {
    ip_event_got_ip6_t *evt = (ip_event_got_ip6_t *)data;
    if (evt->esp_netif != s_backbone_netif ||
        esp_netif_ip6_get_addr_type(&evt->ip6_info.ip) == ESP_IP6_ADDR_IS_LINK_LOCAL) {
        return;
    }
    tcpip_callback(clear_ip6_route_cache, NULL);
    ESP_LOGI(TAG, "Backbone IPv6 address back, IPv6 route cache cleared");
}

static void on_backbone_ready(void) {
    if (initialized) {
        if (s_ip6_back_handler == NULL) {
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6, on_backbone_ip6_back,
                                                NULL, &s_ip6_back_handler);
        }

        // The uplink came back: after a longer outage its IPv6 link-local is gone and nothing else
        // recreates it - border routing (RS/RA) and routes into Thread would stay dead
        esp_ip6_addr_t link_local;
        if (esp_netif_get_ip6_linklocal(s_backbone_netif, &link_local) != ESP_OK) {
            ESP_LOGW(TAG, "Backbone lost its IPv6 link-local, recreating");
            esp_netif_create_ip6_linklocal(s_backbone_netif);
        }
        return;
    }

    if (!s_ot_started) {
        return;
    }

    s_backbone_netif = esp_netif_get_default_netif();
    if (!s_backbone_netif) {
        ESP_LOGE(TAG, "No backbone netif");
        return;
    }

    esp_netif_create_ip6_linklocal(s_backbone_netif);
    ESP_LOGI(TAG, "Waiting for backbone IPv6 link-local...");
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6, on_backbone_ip6, NULL,
                                        &s_ip6_handler);
}

static void thread_border_router_adapter_on_event(EventBits_t bits) {
    if (bits & NETWORK_UPLINK_READY) {
        on_backbone_ready();
    }
}

// What the address is for - OpenThread only tells where it came from ("thread" for most of them)
static const char *thread_address_type(const otNetifAddress *addr) {
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

// Only copying under the OpenThread lock - formatting happens in the tele, without it
static void thread_snapshot_refresh(void) {
    if (!s_ot_started) {
        return;
    }

    // MQTT and HTTP read the snapshot from their own tasks - never while it's half written. The
    // mutex comes first so the snapshot is filled in place, without a second buffer
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);

    // A busy OpenThread lock skips this refresh rather than stall the supervisor - the next one
    // comes later
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
        xSemaphoreGive(s_snapshot_mutex);
        return;
    }

    otInstance *instance = esp_openthread_get_instance();
    memset(&s_snapshot, 0, sizeof(s_snapshot));

    s_snapshot.role = otThreadGetDeviceRole(instance);
    strlcpy(s_snapshot.network, otThreadGetNetworkName(instance), sizeof(s_snapshot.network));
    s_snapshot.channel = otLinkGetChannel(instance);
    s_snapshot.panid = otLinkGetPanId(instance);
    s_snapshot.ext_panid = *otThreadGetExtendedPanId(instance);
    s_snapshot.partition_id = otThreadGetPartitionId(instance);
    otLinkGetFactoryAssignedIeeeEui64(instance, &s_snapshot.eui64);
    s_snapshot.has_ba_id = otBorderAgentGetId(instance, &s_snapshot.ba_id) == OT_ERROR_NONE;
    s_snapshot.has_omr = otBorderRoutingGetOmrPrefix(instance, &s_snapshot.omr) == OT_ERROR_NONE;

    for (const otNetifAddress *addr = otIp6GetUnicastAddresses(instance);
         addr && s_snapshot.addresses_num < THREAD_SNAPSHOT_MAX_ADDRESSES; addr = addr->mNext) {
        s_snapshot.addresses[s_snapshot.addresses_num] = addr->mAddress;
        s_snapshot.address_types[s_snapshot.addresses_num] = thread_address_type(addr);
        s_snapshot.addresses_num++;
    }

    // Devices registered on the BR's SRP server, by the name they announce themselves with
    const otSrpServerHost *host = NULL;
    while ((host = otSrpServerGetNextHost(instance, host)) != NULL &&
           s_snapshot.devices_num < THREAD_SNAPSHOT_MAX_DEVICES) {
        if (otSrpServerHostIsDeleted(host)) {
            continue;
        }

        thread_snapshot_device_t *device = &s_snapshot.devices[s_snapshot.devices_num++];

        // "h2dev.default.service.arpa." - the device name is the first label
        const char *full_name = otSrpServerHostGetFullName(host);
        snprintf(device->name, sizeof(device->name), "%.*s", (int)strcspn(full_name, "."),
                 full_name);

        uint8_t addresses_num = 0;
        const otIp6Address *addresses = otSrpServerHostGetAddresses(host, &addresses_num);
        if (addresses_num > 0) {
            device->ip = addresses[0];
            device->has_ip = true;
        }
    }

    esp_openthread_lock_release();

    s_snapshot_valid = true;
    xSemaphoreGive(s_snapshot_mutex);
}

// The network changes rarely, so 10 s is plenty
static void thread_border_router_adapter_on_interval(supervisor_interval_stage_t stage) {
    if (stage == SUPERVISOR_INTERVAL_10S) {
        thread_snapshot_refresh();
    }
}

static void tele_add_hex(cJSON *json, const char *key, const uint8_t *bytes, size_t len) {
    char hex[2 * OT_BORDER_AGENT_ID_LENGTH + 1];
    for (size_t i = 0; i < len && i < OT_BORDER_AGENT_ID_LENGTH; i++) {
        snprintf(&hex[2 * i], 3, "%02x", bytes[i]);
    }
    cJSON_AddStringToObject(json, key, hex);
}

static void tele_thread(const char *tele_id, cJSON *json_root) {
    if (s_snapshot_mutex == NULL ||
        xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    if (!s_snapshot_valid) {
        xSemaphoreGive(s_snapshot_mutex);
        return;
    }

    const thread_snapshot_t *snap = &s_snapshot;
    cJSON *thread = cJSON_AddObjectToObject(json_root, tele_id);
    char buf[OT_IP6_PREFIX_STRING_SIZE];

    cJSON_AddStringToObject(thread, "role", otThreadDeviceRoleToString(snap->role));
    cJSON_AddStringToObject(thread, "network", snap->network);
    cJSON_AddNumberToObject(thread, "channel", snap->channel);

    if (s_tx_power_known) {
        cJSON_AddNumberToObject(thread, "tx_power", s_tx_power);
    }

    snprintf(buf, sizeof(buf), "0x%04x", snap->panid);
    cJSON_AddStringToObject(thread, "panid", buf);
    tele_add_hex(thread, "ext_panid", snap->ext_panid.m8, sizeof(snap->ext_panid.m8));
    cJSON_AddNumberToObject(thread, "partition_id", snap->partition_id);
    tele_add_hex(thread, "eui64", snap->eui64.m8, sizeof(snap->eui64.m8));

    if (snap->has_ba_id) {
        tele_add_hex(thread, "ba_id", snap->ba_id.mId, sizeof(snap->ba_id.mId));
    }

    if (snap->has_omr) {
        otIp6PrefixToString(&snap->omr, buf, sizeof(buf));
        cJSON_AddStringToObject(thread, "omr", buf);
    }

    cJSON *addresses = cJSON_AddArrayToObject(thread, "addresses");
    for (uint8_t i = 0; i < snap->addresses_num; i++) {
        cJSON *address = cJSON_CreateObject();
        otIp6AddressToString(&snap->addresses[i], buf, sizeof(buf));
        cJSON_AddStringToObject(address, "ip", buf);
        if (snap->address_types[i]) {
            cJSON_AddStringToObject(address, "type", snap->address_types[i]);
        }
        cJSON_AddItemToArray(addresses, address);
    }

    cJSON *devices = cJSON_AddArrayToObject(thread, "devices");
    for (uint8_t i = 0; i < snap->devices_num; i++) {
        cJSON *device = cJSON_CreateObject();
        cJSON_AddStringToObject(device, "name", snap->devices[i].name);
        if (snap->devices[i].has_ip) {
            otIp6AddressToString(&snap->devices[i].ip, buf, sizeof(buf));
            cJSON_AddStringToObject(device, "ip", buf);
        }
        cJSON_AddItemToArray(devices, device);
    }

    thread_commissioner_tele(thread);

    xSemaphoreGive(s_snapshot_mutex);
}

static const tele_entry_t thread_border_router_tele[] = {{"thread", tele_thread}, {NULL, NULL}};

supervisor_platform_adapter_t thread_border_router_adapter = {
    .name = "thread_border_router",
    .enable_in_safe_mode = false,
    .init = thread_border_router_adapter_init,
    .shutdown = thread_border_router_adapter_shutdown,
    .on_event = thread_border_router_adapter_on_event,
    .on_interval = thread_border_router_adapter_on_interval,
    .cmnd_group = thread_commands,
    .tele_group = thread_border_router_tele,
};
