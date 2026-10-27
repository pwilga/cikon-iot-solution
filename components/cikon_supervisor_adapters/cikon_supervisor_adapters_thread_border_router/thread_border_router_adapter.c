#include "thread_border_router_adapter.h"

#if CONFIG_THREAD_BORDER_ROUTER_WEB
#include "esp_br_web.h"
#endif

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
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
#include "openthread/border_routing.h"
#include "openthread/dataset_ftd.h"
#include "openthread/thread.h"

#include "bits_helper.h"
#include "config_manager.h"
#include "inet_common.h"
#include "supervisor.h"
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

static const esp_openthread_config_t s_ot_config = THREAD_DEFAULT_OT_CONFIG();

// Thread starts without waiting for the backbone, border routing joins later
static void thread_start_task(void *arg) {

    esp_openthread_lock_acquire(portMAX_DELAY);

    otOperationalDatasetTlvs dataset;
    bool dataset_ready = false;

    if (CONFIG_THREAD_BORDER_ROUTER_PROVISIONED_DATASET[0] != '\0') {
        if (thread_dataset_parse_hex(CONFIG_THREAD_BORDER_ROUTER_PROVISIONED_DATASET, &dataset)) {
            ESP_LOGI(TAG, "Using provisioned dataset");
            dataset_ready = true;
        } else {
            ESP_LOGW(TAG, "Provisioned dataset parse failed, falling back to NVS/new");
        }
    }

    if (!dataset_ready) {
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
    }

    esp_openthread_auto_start(&dataset);
    esp_openthread_lock_release();
    ESP_LOGI(TAG, "Thread network started");

done:
    vTaskDelete(NULL);
}

static esp_err_t thread_border_router_adapter_init(void) {
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_THREAD_BORDER_ROUTER_WEB
    // esp_br_web_start() registers an mDNS hostname-changed callback and aborts if mDNS
    // isn't running yet. The inet adapter also brings mDNS up (on IP), but that happens
    // after us — ensure it here. inet_common_mdns_init() is idempotent.
    inet_common_mdns_init();
    esp_br_web_start(CONFIG_VFS_SPIFFS_MOUNT_POINT);
    ESP_LOGI(TAG, "Thread BR Web GUI handler registered");
#endif

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

supervisor_platform_adapter_t thread_border_router_adapter = {
    .name = "thread_border_router",
    .enable_in_safe_mode = false,
    .init = thread_border_router_adapter_init,
    .shutdown = thread_border_router_adapter_shutdown,
    .on_event = thread_border_router_adapter_on_event,
};
