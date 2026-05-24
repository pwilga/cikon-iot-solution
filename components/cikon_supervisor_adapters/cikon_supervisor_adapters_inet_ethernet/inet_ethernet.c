/*
 * SPDX-License-Identifier: MIT
 *
 * Cikon IoT Supervisor - Ethernet Network Adapter
 * Copyright (c) 2026 Piotr Wilga
 */

#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#include <string.h>
#include <time.h>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"

#include "bits_helper.h"
#include "cmnd.h"
#include "config_manager.h"
#include "ethernet.h"
#include "inet_common.h"
#include "inet_ethernet_adapter.h"
#include "json_parser.h"
#include "metadata.h"
#include "mqtt.h"
#include "platform_services.h"
#include "supervisor.h"
#include "tcp_monitor.h"
#include "tcp_ota.h"
#include "tele.h"

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
#include "ha.h"
#endif

#define TAG "cikon:adapter:inet_ethernet"

static bool initialized = false;
static bool services_running = false;
static bool last_internet_reachable = false;

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;

static esp_event_handler_instance_t inet_eth_handler = NULL;
static esp_event_handler_instance_t inet_ip_handler = NULL;

static bool shutdown_ota = true;

// ===== Lifecycle Management =====

static void inet_ethernet_stop_services(void) {
    if (!services_running) {
        ESP_LOGD(TAG, "Services not running");
        return;
    }

    ESP_LOGI(TAG, "Stopping network services");

    // CRITICAL: Shutdown tcp_monitor FIRST before network stack resets
    tcp_monitor_shutdown();

    mqtt_publish_offline_state();
    mqtt_shutdown();
    inet_common_mdns_shutdown();
    esp_netif_sntp_deinit();

    if (shutdown_ota) {
        tcp_ota_shutdown();
    }

    services_running = false;
}

static void inet_ethernet_restart_cb(void) {
    shutdown_ota = false; // Don't shutdown OTA before restart

    // SAFE MODE: Only clear boot counter
    if (supervisor_is_safe_mode_active()) {
        ESP_LOGD(TAG, "SAFE MODE: Clearing boot counter before restart");
        config_set_boot_counter(0);
        vTaskDelay(pdMS_TO_TICKS(500)); // Ensure NVS write completes
        return;
    }

    // NORMAL MODE: Full shutdown sequence
    mqtt_publish_offline_state();
    mqtt_shutdown();
}

static void inet_ethernet_sntp_sync_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "SNTP time synchronized");
    supervisor_notify_event(INET_EVENT_TIME_SYNCED);
}

// ===== Event Handlers =====

static void inet_ethernet_netif_event_handler(void *arg, esp_event_base_t event_base,
                                              int32_t event_id, void *event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Ethernet Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        supervisor_notify_event(INET_EVENT_STA_READY); // Reuse STA_READY event
    } else if (event_base == ETHERNET_EVENT) {
        switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Down");
            supervisor_notify_event(INET_EVENT_STA_LOST); // Reuse STA_LOST event
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGD(TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGD(TAG, "Ethernet Stopped");
            break;
        }
    }
}

// ===== Adapter Lifecycle =====

static esp_err_t inet_ethernet_adapter_init(void) {
    if (initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing Ethernet network adapter");

    // Step 1: Initialize TCP/IP network interface (once)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Step 2: Initialize Ethernet hardware (cikon_ethernet)
    esp_err_t ret = ethernet_init(&s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet hardware init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Step 3: Create network interface for Ethernet
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&cfg);
    if (s_eth_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create esp_netif");
        ethernet_shutdown(s_eth_handle);
        return ESP_FAIL;
    }

    // Step 4: Attach Ethernet driver to TCP/IP stack
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, s_eth_glue));

    // Step 5: Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, &inet_ethernet_netif_event_handler, NULL, &inet_eth_handler));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, &inet_ethernet_netif_event_handler, NULL, &inet_ip_handler));

    // Step 6: Register platform callbacks
    set_restart_callback(inet_ethernet_restart_cb);
    inet_common_sntp_set_sync_cb(inet_ethernet_sntp_sync_cb);

    // Step 7: Start Ethernet driver
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

    initialized = true;
    ESP_LOGI(TAG, "Ethernet adapter initialized, waiting for IP...");
    return ESP_OK;
}

static esp_err_t inet_ethernet_adapter_shutdown(void) {
    if (!initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Shutting down Ethernet adapter");

    // Step 1: Stop Ethernet driver
    if (s_eth_handle) {
        esp_eth_stop(s_eth_handle);
    }

    // Step 2: Unregister event handlers
    if (inet_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, inet_ip_handler);
        inet_ip_handler = NULL;
    }

    if (inet_eth_handler) {
        esp_event_handler_instance_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, inet_eth_handler);
        inet_eth_handler = NULL;
    }

    // Step 3: Stop all services
    inet_ethernet_stop_services();

    // Step 4: Unregister callbacks
    set_restart_callback(NULL);

    // Step 5: Cleanup network interface
    if (s_eth_glue) {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }

    if (s_eth_netif) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }

    // Step 6: Shutdown Ethernet hardware
    if (s_eth_handle) {
        ethernet_shutdown(s_eth_handle);
        s_eth_handle = NULL;
    }

    shutdown_ota = true;
    initialized = false;

    ESP_LOGI(TAG, "Ethernet adapter shutdown complete");
    return ESP_OK;
}

static void inet_ethernet_adapter_on_event(EventBits_t bits) {

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    if (bits & SUPERVISOR_EVENT_PLATFORM_INITIALIZED) {
        // Register all adapters' HA entities from metadata
        inet_common_register_all_ha_entities();

        // Register inet_ethernet's own HA entity
        ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                                 .name = "IP",
                                                 .icon = "mdi:ip-outline",
                                                 .entity_category = "diagnostic"});
    }
#endif

    if (bits & SUPERVISOR_EVENT_CMND_COMPLETED) {
        mqtt_trigger_telemetry();
    }

    if (bits & INET_EVENT_TIME_SYNCED) {
        time_t now_sec = 0;
        time(&now_sec);

        struct tm tm_now = {0};
        localtime_r(&now_sec, &tm_now);

        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_now);
        ESP_LOGW(TAG, "Time synced: %s", buf);
    }

    if (bits & INET_EVENT_STA_READY) { // Got IP
        if (services_running) {
            ESP_LOGW(TAG, "Services already running, ignoring");
            if (!supervisor_is_safe_mode_active()) {
                mqtt_init();
            }
            return;
        }

        ESP_LOGI(TAG, "Ethernet ready, starting services");
        tcp_ota_init();
        tcp_monitor_init();

        if (!supervisor_is_safe_mode_active()) {
            inet_common_mdns_init();
            inet_common_sntp_init();
            mqtt_init();
        }

        services_running = true;
    }

    if (bits & INET_EVENT_STA_LOST) { // Link down
        ESP_LOGW(TAG, "Ethernet link lost");
        inet_ethernet_stop_services();
    }
}

static void inet_ethernet_adapter_on_interval(supervisor_interval_stage_t stage) {
    switch (stage) {
    case SUPERVISOR_INTERVAL_1S:
        break;

    case SUPERVISOR_INTERVAL_5S: {
        bool current_state = is_internet_reachable();

        // Only notify on state change
        if (current_state != last_internet_reachable) {
            if (current_state) {
                supervisor_notify_event(INET_INTERNET_READY);
            } else {
                supervisor_notify_event(INET_INTERNET_LOST);
            }
            last_internet_reachable = current_state;
        }
        break;
    }

    case SUPERVISOR_INTERVAL_10M:
        if (services_running) {
            mqtt_init(); // Reconnect MQTT if needed
        }
        break;

    default:
        break;
    }
}

// ===== Command Handlers =====

static void sntp_handler(const char *args_json_str) {
    logic_state_t sntp_state = json_str_as_logic_state(args_json_str);

    if (sntp_state == STATE_ON) {
        ESP_LOGI(TAG, "Starting SNTP service");
        inet_common_sntp_init();
    } else if (sntp_state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping SNTP service");
        esp_netif_sntp_deinit();
    } else {
        ESP_LOGW(TAG, "Invalid SNTP state");
    }
}

static void ota_handler(const char *args_json_str) {
    logic_state_t ota_state = json_str_as_logic_state(args_json_str);

    if (ota_state == STATE_ON) {
        ESP_LOGI(TAG, "Starting OTA service");
        tcp_ota_init();
    } else if (ota_state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping OTA service");
        tcp_ota_shutdown();
    }
}

static void monitor_handler(const char *args_json_str) {
    logic_state_t monitor_state = json_str_as_logic_state(args_json_str);

    if (monitor_state == STATE_ON) {
        ESP_LOGI(TAG, "Starting TCP monitor");
        tcp_monitor_init();
    } else if (monitor_state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping TCP monitor");
        tcp_monitor_shutdown();
    }
}

// ===== Telemetry =====

static void tele_inet_ethernet_ip_address(const char *tele_id, cJSON *json_root) {
    if (!s_eth_netif) {
        cJSON_AddStringToObject(json_root, tele_id, "N/A");
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_eth_netif, &ip_info) == ESP_OK) {
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        cJSON_AddStringToObject(json_root, tele_id, ip_str);
    } else {
        cJSON_AddStringToObject(json_root, tele_id, "0.0.0.0");
    }
}

static void tele_inet_ethernet_backend(const char *tele_id, cJSON *json_root) {
    const char *backend = ethernet_get_backend_name();
    cJSON_AddStringToObject(json_root, tele_id, backend);
}

// ===== Adapter Registration =====

static const command_entry_t inet_ethernet_commands[] = {
    {"sntp", "Control SNTP service (on/off)", sntp_handler},
    {"ota", "Control OTA service (on/off)", ota_handler},
    {"monitor", "Control TCP monitor (on/off)", monitor_handler},
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    {"ha", "Trigger Home Assistant MQTT discovery", inet_common_ha_discovery_handler},
#endif
    {NULL, NULL, NULL}};

static const tele_entry_t inet_ethernet_telemetry[] = {{"ip", tele_inet_ethernet_ip_address},
                                                       {"eth_backend", tele_inet_ethernet_backend},
                                                       {NULL, NULL}};

supervisor_platform_adapter_t inet_ethernet_adapter = {
    .enable_in_safe_mode = true, // Critical: always init inet adapter
    .name = "inet_ethernet",
    .init = inet_ethernet_adapter_init,
    .shutdown = inet_ethernet_adapter_shutdown,
    .on_event = inet_ethernet_adapter_on_event,
    .on_interval = inet_ethernet_adapter_on_interval,
    .cmnd_group = inet_ethernet_commands,
    .tele_group = inet_ethernet_telemetry,
};
