#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_wifi_types_generic.h"
#include "mdns.h"

#include "cmnd.h"
#include "config_manager.h"
#include "metadata.h"
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
#include "ha.h"
#endif
#include "https_server.h"
#include "inet_adapter.h"
#include "json_parser.h"
#include "mqtt.h"
#include "platform_services.h"
#include "supervisor.h"
#include "tcp_monitor.h"
#include "tcp_ota.h"
#include "tele.h"
#include "wifi.h"

#define TAG "cikon:adapter:inet"

#define INET_EVENT_TIME_SYNCED BIT4
#define INET_EVENT_STA_READY BIT5
#define INET_EVENT_AP_READY BIT6
#define INET_EVENT_RESERVED BIT7

// Forward declaration for wifi command handler (used in inet_adapter_init)
static void wifi_handler(const char *args_json_str);

static const char *mdns_host = NULL;
static const char *mdns_instance = NULL;

static const char *sntp_servers[CONFIG_LWIP_SNTP_MAX_SERVERS] = {NULL};
static esp_sntp_time_cb_t sntp_cb = NULL;

static esp_event_handler_instance_t inet_wifi_handler = NULL;
static esp_event_handler_instance_t inet_ip_handler = NULL;

static bool shutdown_ota = true;

static SemaphoreHandle_t network_transition_mutex = NULL;
static volatile bool sta_services_running = false;
static volatile bool ap_services_running = false;

static void inet_mdns_configure(const char *hostname, const char *instance_name) {
    mdns_host = hostname;
    mdns_instance = instance_name;
}

static void inet_mdns_init(void) {

    esp_err_t ret = (mdns_init() != ESP_OK || mdns_hostname_set(mdns_host) != ESP_OK ||
                     mdns_instance_name_set(mdns_instance) != ESP_OK)
                        ? ESP_FAIL
                        : ESP_OK;

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "mDNS started with hostname: %s.local", mdns_host);
    } else {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
    }
}

static void inet_sntp_configure(const char **servers, esp_sntp_time_cb_t cb) {
    for (int i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; ++i) {
        if (servers[i] && strlen(servers[i]) > 0) {
            sntp_servers[i] = servers[i];
        } else {
            sntp_servers[i] = NULL;
        }
    }
    sntp_cb = cb;
}

static void inet_sntp_init(void) {
    if (memcmp(sntp_servers, (const char *[CONFIG_LWIP_SNTP_MAX_SERVERS]){NULL},
               sizeof(sntp_servers)) == 0) {
        ESP_LOGW(TAG, "No SNTP servers configured, skipping SNTP init.");
        return;
    }

    esp_sntp_config_t config = {
        .smooth_sync = false,
        .server_from_dhcp = false,
        .wait_for_sync = true,
        .start = true,
        .sync_cb = sntp_cb,
        .renew_servers_after_new_IP = false,
        .ip_event_to_renew = IP_EVENT_STA_GOT_IP,
        .index_of_first_server = 0,
        .num_of_servers = CONFIG_LWIP_SNTP_MAX_SERVERS,
        .servers = {NULL},
    };

    for (int i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; ++i) {
        config.servers[i] = sntp_servers[i];
    }

    esp_netif_sntp_init(&config);
    ESP_LOGI(TAG, "SNTP service initialized, servers: %s, %s, %s",
             config.servers[0] ? config.servers[0] : "none",
             config.servers[1] ? config.servers[1] : "none",
             config.servers[2] ? config.servers[2] : "none");
}

static void inet_stop_services(void) {
    ESP_LOGI(TAG, "Stopping network services");

    // CRITICAL: Shutdown tcp_monitor FIRST before network stack resets
    tcp_monitor_shutdown();

    mqtt_publish_offline_state();
    mqtt_shutdown();
    https_shutdown();
    // Keep mDNS running - it works for both STA and AP interfaces
    // mdns_free();
    esp_netif_sntp_deinit();

    if (shutdown_ota) {
        tcp_ota_shutdown();
    }

    sta_services_running = false;
    ap_services_running = false;
}

void inet_switch_to_ap_mode(void) {
    if (xSemaphoreTake(network_transition_mutex, pdMS_TO_TICKS(100)) == pdFALSE) {
        ESP_LOGW(TAG, "Network transition already in progress, cannot switch to AP");
        return;
    }

    // ESP_LOGI(TAG, "Switching to AP mode");
    inet_stop_services();
    wifi_init_ap_mode();

    xSemaphoreGive(network_transition_mutex);
}

void inet_switch_to_sta_mode(void) {
    if (xSemaphoreTake(network_transition_mutex, pdMS_TO_TICKS(100)) == pdFALSE) {
        ESP_LOGW(TAG, "Network transition already in progress, cannot switch to STA");
        return;
    }

    // ESP_LOGI(TAG, "Switching to STA mode");
    inet_stop_services();
    wifi_init_sta_mode();

    xSemaphoreGive(network_transition_mutex);
}

static void inet_sntp_sync_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "SNTP time synchronized");
    supervisor_notify_event(INET_EVENT_TIME_SYNCED);
}

static void inet_restart_cb(void) {
    // Unregister event handlers first (safe for both modes)
    wifi_unregister_event_handlers();

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

static void inet_netif_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                     void *event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        supervisor_notify_event(INET_EVENT_STA_READY);
    }

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            supervisor_notify_event(INET_EVENT_AP_READY);
            break;
        case WIFI_EVENT_AP_STOP:
            // ESP_LOGI(TAG, "AP stopped");
            // Services stopped via callback before mode switch
            break;
        }
    }
}

static const char *inet_get_or_generate_ap_ssid(void) {
    const char *cfg_ssid = config_get()->wifi_ap_ssid;
    if (cfg_ssid && strlen(cfg_ssid) > 0) {
        return cfg_ssid;
    }

    static char ssid[33];
    const char *mac_str = get_client_id();
    const char *mac_last6 = mac_str + 6;

    snprintf(ssid, sizeof(ssid), "%s_%s", config_get()->dev_name, mac_last6);
    return ssid;
}

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
static void inet_adapter_register_ha_entities(void) {
    // Register inet's own HA entity
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "IP",
                                             .icon = "mdi:ip-outline",
                                             .entity_category = "diagnostic"});

    // Iterate through all adapters and register their HA metadata
    const supervisor_platform_adapter_t **adapters = supervisor_get_adapters();
    for (int i = 0; adapters[i] != NULL; i++) {
        if (adapters[i]->metadata == NULL) {
            continue;
        }

        const ha_metadata_t *meta = (const ha_metadata_t *)adapters[i]->metadata;

        // Check if this is HA metadata (magic signature)
        if (meta->magic != HA_METADATA_MAGIC) {
            continue;
        }

        // Register all entities from this adapter
        for (int e = 0; meta->entities[e].type != HA_ENTITY_NONE; e++) {
            ha_register_entity(&meta->entities[e]);
        }
    }
}
#else
static inline void inet_adapter_register_ha_entities(void) {}
#endif

void inet_adapter_init(void) {

    if (is_wifi_network_connected()) {
        ESP_LOGW(TAG, "WiFi network already connected, skipping inet adapter init");
        return;
    }

    ESP_LOGI(TAG, "Initializing inet adapter");

    if (network_transition_mutex == NULL) {
        network_transition_mutex = xSemaphoreCreateMutex();
        if (!network_transition_mutex) {
            ESP_LOGE(TAG, "Failed to create network transition mutex!");
        }
    }

    wifi_credentials_t creds = {.sta_ssid = config_get()->wifi_ssid,
                                .sta_password = config_get()->wifi_pass,
                                .ap_ssid = inet_get_or_generate_ap_ssid(),
                                .ap_password = NULL};

    wifi_configure(&creds);

    const char *hostname = config_get()->mdns_host;
    if (strlen(hostname) == 0) {
        hostname = config_get()->dev_name;
    }

    inet_mdns_configure(hostname, config_get()->mdns_instance);

    inet_sntp_configure(
        (const char *[]){config_get()->sntp1, config_get()->sntp2, config_get()->sntp3},
        inet_sntp_sync_cb);

    static char device_url[64];

    // TODO: Ethernet support ? Check both interfaces
    snprintf(device_url, sizeof(device_url), "%s.local", hostname);

    mqtt_config_t mqtt_cfg = {.client_id = get_client_id(),
                              .device_name = config_get()->dev_name,
                              .device_manufacturer = "Cikon Systems",
                              .device_model = CONFIG_IDF_TARGET,
                              .device_sw_version = "v1.0.0",
                              .device_hw_version = CONFIG_IDF_INIT_VERSION,
                              .device_uri = device_url,
                              .mqtt_node = config_get()->mqtt_node,
                              .mqtt_broker = config_get()->mqtt_broker,
                              .mqtt_user = config_get()->mqtt_user,
                              .mqtt_pass = config_get()->mqtt_pass,
                              .mqtt_mtls_en = config_get()->mqtt_mtls_en,
                              .mqtt_max_retry = config_get()->mqtt_max_retry,
                              .mqtt_disc_pref = config_get()->mqtt_disc_pref,
                              .command_cb = cmnd_process_json,
                              .telemetry_cb = tele_append_all};

    mqtt_configure(&mqtt_cfg);

    static const https_endpoint_config_t inet_https_endpoints[] = {
        {.uri = "/cmnd", .method = HTTP_POST, .json_cmnd = cmnd_process_json},
        {.uri = "/tele", .method = HTTP_GET, .json_tele = tele_append_all},
        {.uri = NULL} // sentinel
    };

    https_configure(inet_https_endpoints, config_get()->http_auth);

    set_restart_callback(inet_restart_cb);

    // Register callback for AP timeout -> STA mode switch
    wifi_set_ap_timeout_callback(inet_stop_services);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &inet_netif_event_handler, NULL, &inet_wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &inet_netif_event_handler, NULL, &inet_ip_handler));

    wifi_init_sta_mode();

    // Register persistent wifi command (not unregistered with adapter shutdown)
    if (!cmnd_find("wifi")) {
        cmnd_register("wifi", "Control WiFi (on/off)", wifi_handler);
    }
}

void inet_adapter_shutdown(void) {

    ESP_LOGI(TAG, "Shutting down inet platform adapter");

    if (inet_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, inet_ip_handler);
        inet_ip_handler = NULL;
    }

    if (inet_wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, inet_wifi_handler);
        inet_wifi_handler = NULL;
    }

    inet_stop_services();
    mdns_free();

    // Unregister callbacks
    set_restart_callback(NULL);
    wifi_set_ap_timeout_callback(NULL);

    if (network_transition_mutex) {
        vSemaphoreDelete(network_transition_mutex);
        network_transition_mutex = NULL;
    }

    shutdown_ota = true;

    wifi_shutdown();
}

static void inet_adapter_on_event(EventBits_t bits) {

    if (bits & SUPERVISOR_EVENT_PLATFORM_INITIALIZED) {
        // All adapters initialized, register HA entities from metadata
        inet_adapter_register_ha_entities();
    }

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

    if (bits & INET_EVENT_STA_READY) {

        if (sta_services_running) {
            ESP_LOGW(TAG, "STA services already running, ignoring");
            if (!supervisor_is_safe_mode_active()) {
                mqtt_init();
            }
            return;
        }

        ESP_LOGI(TAG, "STA ready, starting services");
        tcp_ota_init();
        tcp_monitor_init();

        if (!supervisor_is_safe_mode_active()) {
            inet_mdns_init();
            inet_sntp_init();
            mqtt_init();
        }

        sta_services_running = true;
        ap_services_running = false;
    }

    if (bits & INET_EVENT_AP_READY) {
        if (ap_services_running) {
            ESP_LOGW(TAG, "AP services already running, ignoring duplicate event");
            return;
        }

        ESP_LOGI(TAG, "AP ready, starting services");
        tcp_ota_init();
        tcp_monitor_init();

        if (!supervisor_is_safe_mode_active()) {
            https_init();
            inet_mdns_init();
        }

        ap_services_running = true;
        sta_services_running = false;
    }
}

static void inet_adapter_on_interval(supervisor_interval_stage_t stage) {
    switch (stage) {
    case SUPERVISOR_INTERVAL_1S:
        break;

    case SUPERVISOR_INTERVAL_5S:
        break;

    case SUPERVISOR_INTERVAL_60S:
        break;

    case SUPERVISOR_INTERVAL_5M:
        break;

    case SUPERVISOR_INTERVAL_10M:
        if (sta_services_running) {
            mqtt_init();
        }
        break;

    case SUPERVISOR_INTERVAL_2H:
        break;

    case SUPERVISOR_INTERVAL_12H:
        break;

    default:
        break;
    }
}

static void set_ap_handler(const char *args_json_str) {
    (void)args_json_str;
    inet_switch_to_ap_mode();
}

static void set_sta_handler(const char *args_json_str) {
    (void)args_json_str;
    inet_switch_to_sta_mode();
}

static void https_handler(const char *args_json_str) {
    logic_state_t https_state = json_str_as_logic_state(args_json_str);

    if (https_state == STATE_ON) {
        ESP_LOGI(TAG, "Starting HTTPS server");
        https_init();
    } else if (https_state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping HTTPS server");
        https_shutdown();
    } else {
        ESP_LOGW(TAG, "Invalid HTTPS state");
    }
}

static void sntp_handler(const char *args_json_str) {
    logic_state_t sntp_state = json_str_as_logic_state(args_json_str);

    if (sntp_state == STATE_ON) {
        ESP_LOGI(TAG, "Starting SNTP service");
        esp_netif_sntp_deinit();
    } else if (sntp_state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping SNTP service");
        esp_netif_sntp_deinit();
    } else {
        ESP_LOGW(TAG, "Invalid SNTP state");
    }
}

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
static void ha_handler(const char *args_json_str) {
    logic_state_t force_empty_payload = json_str_as_logic_state(args_json_str);
    if (force_empty_payload == STATE_TOGGLE) {
        ESP_LOGE(TAG, "Toggling is not permitted for HA discovery");
        return;
    }

    ESP_LOGI(TAG, "Triggering Home Assistant MQTT discovery");
    publish_ha_mqtt_discovery(force_empty_payload == STATE_OFF);
}
#endif

static void ota_handler(const char *args_json_str) {
    logic_state_t ota_state = json_str_as_logic_state(args_json_str);

    if (ota_state == STATE_ON) {
        ESP_LOGI(TAG, "Starting OTA update");
        tcp_ota_init();
    } else if (ota_state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping OTA update");
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

static void wifi_handler(const char *args_json_str) {
    logic_state_t wifi_state = json_str_as_logic_state(args_json_str);

    if (wifi_state == STATE_ON) {
        ESP_LOGI(TAG, "Starting WiFi");
        inet_adapter.init();
    } else if (wifi_state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping WiFi");
        inet_adapter.shutdown();
    }
}

static void tele_inet_ip_address(const char *tele_id, cJSON *json_root) {
    char ip[16];
    wifi_get_interface_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(json_root, tele_id, ip);
}

static const command_entry_t inet_commands[] = {
    {"ap", "Switch to AP mode", set_ap_handler},
    {"sta", "Switch to STA mode", set_sta_handler},
    {"https", "Control HTTPS server (on/off)", https_handler},
    {"sntp", "Control SNTP service (on/off)", sntp_handler},
    {"ota", "Control OTA service (on/off)", ota_handler},
    {"monitor", "Control TCP monitor (on/off)", monitor_handler},
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    {"ha", "Trigger Home Assistant MQTT discovery", ha_handler},
#endif
    {NULL, NULL, NULL}};

supervisor_platform_adapter_t inet_adapter = {
    .enable_in_safe_mode = true, // Critical: always init inet adapter
    .init = inet_adapter_init,
    .shutdown = inet_adapter_shutdown,
    .on_event = inet_adapter_on_event,
    .on_interval = inet_adapter_on_interval,
    .cmnd_group = inet_commands,
    .tele_group = (const tele_entry_t[]){{"ip", tele_inet_ip_address}, {NULL, NULL}},
};
