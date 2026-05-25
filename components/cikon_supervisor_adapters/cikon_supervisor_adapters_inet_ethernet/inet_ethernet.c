#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#include "esp_eth.h" // IWYU pragma: keep
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

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

static esp_event_handler_instance_t inet_eth_handler = NULL;
static esp_event_handler_instance_t inet_ip_handler = NULL;

static bool shutdown_ota = true;

static void inet_ethernet_stop_services(void) {
    if (!services_running) {
        ESP_LOGD(TAG, "Services not running");
        return;
    }

    ESP_LOGI(TAG, "Stopping network services");

    // CRITICAL: Shutdown tcp_monitor FIRST before network stack resets
    tcp_monitor_shutdown();

    mqtt_publish_offline_state();

    inet_common_mqtt_shutdown();
    inet_common_mdns_shutdown();
    inet_common_sntp_shutdown();

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

    mqtt_publish_offline_state();
    inet_common_mqtt_shutdown();
}

static void inet_ethernet_netif_event_handler(void *arg, esp_event_base_t event_base,
                                              int32_t event_id, void *event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Ethernet Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        supervisor_notify_event(INET_ETH_READY);
    } else if (event_base == ETH_EVENT) {
        switch (event_id) {
        case ETHERNET_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "Ethernet Link Up");
            // CRITICAL: Start DHCP client (not automatic like WiFi!)
            esp_netif_t *eth_netif = ethernet_get_netif();
            if (eth_netif) {
                esp_err_t ret = esp_netif_dhcpc_start(eth_netif);
                if (ret == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                    ESP_LOGD(TAG, "DHCP client already started");
                } else if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to start DHCP client: %s", esp_err_to_name(ret));
                } else {
                    ESP_LOGI(TAG, "DHCP client started");
                }
            }
            break;
        }
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Down");
            supervisor_notify_event(INET_ETH_LOST);
            break;
        case ETHERNET_EVENT_START:
            // ESP_LOGD(TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            // ESP_LOGD(TAG, "Ethernet Stopped");
            break;
        }
    }
}

static esp_err_t inet_ethernet_adapter_init(void) {
    if (initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing Ethernet network adapter");

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, &inet_ethernet_netif_event_handler, NULL, &inet_eth_handler));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, &inet_ethernet_netif_event_handler, NULL, &inet_ip_handler));

    esp_err_t ret = ethernet_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet stack init failed: %s", esp_err_to_name(ret));
        if (inet_ip_handler) {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, inet_ip_handler);
            inet_ip_handler = NULL;
        }
        if (inet_eth_handler) {
            esp_event_handler_instance_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, inet_eth_handler);
            inet_eth_handler = NULL;
        }
        return ret;
    }

    set_restart_callback(inet_ethernet_restart_cb);

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

    if (inet_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, inet_ip_handler);
        inet_ip_handler = NULL;
    }

    if (inet_eth_handler) {
        esp_event_handler_instance_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, inet_eth_handler);
        inet_eth_handler = NULL;
    }

    inet_ethernet_stop_services();
    set_restart_callback(NULL);
    ethernet_shutdown();

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

    if (bits & INET_ETH_READY) { // Got IP
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
            inet_common_mqtt_init();
        }

        services_running = true;
    }

    if (bits & INET_ETH_LOST) { // Link down
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

static void sntp_handler(const char *args_json_str) {
    logic_state_t sntp_state = json_str_as_logic_state(args_json_str);

    if (sntp_state == STATE_ON) {
        ESP_LOGI(TAG, "Starting SNTP service");
        inet_common_sntp_init();
    } else if (sntp_state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping SNTP service");
        inet_common_sntp_shutdown();
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

static void tele_inet_ethernet_ip_address(const char *tele_id, cJSON *json_root) {
    char ip_str[16];
    ethernet_get_interface_ip(ip_str, sizeof(ip_str));
    cJSON_AddStringToObject(json_root, tele_id, ip_str);
}

static void tele_inet_ethernet_backend(const char *tele_id, cJSON *json_root) {
    const char *backend = ethernet_get_backend_name();
    cJSON_AddStringToObject(json_root, tele_id, backend);
}

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
