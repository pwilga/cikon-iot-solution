#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#if !CONFIG_NETWORK_UPLINK_THREAD
#include "mdns.h"
#endif

#include "bits_helper.h"
#include "cmnd.h"
#include "config_manager.h"
#include "json_parser.h"
#include "mqtt.h"
#include "platform_services.h"
#include "supervisor.h"
#include "tele.h"
#include "time_helpers.h"
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
#include "ha.h"
#include "metadata.h"
#endif

#include "esp_netif.h"
#include "http_server.h"
#include "network_services.h"
#include "tcp_monitor.h"
#include "tcp_ota.h"

#if CONFIG_NETWORK_UPLINK_THREAD
#include "thread_common.h"
#endif

#define TAG "cikon:network:services"

static char s_device_url[64];
static bool s_mdns_ready = false;

static void sntp_sync_callback(struct timeval *tv) {
    char time_str[32];
    format_current_time(time_str, sizeof(time_str), NULL);
    ESP_LOGW(TAG, "SNTP time synchronized: %s", time_str);
}

void network_services_mqtt_init(void) {
    const char *device_url = network_services_get_device_url();
    const device_info_t *dev_info = get_device_info();

    mqtt_config_t mqtt_cfg = {
        .client_id = dev_info->id,
        .device_name = config_get()->dev_name,
        .device_manufacturer = "Cikon Systems",
        .device_model = dev_info->chip,
        .device_sw_version = dev_info->app_version,
        .device_hw_version = dev_info->idf_version,
        .device_uri = device_url,
        .mqtt_node = config_get()->mqtt_node,
        .mqtt_broker = config_get()->mqtt_broker,
        .mqtt_user = config_get()->mqtt_user,
        .mqtt_pass = config_get()->mqtt_pass,
        .mqtt_mtls_en = config_get()->mqtt_mtls_en,
        .mqtt_max_retry = config_get()->mqtt_max_retry,
        .mqtt_disc_pref = config_get()->mqtt_disc_pref,
        .command_cb = cmnd_process_json,
        .telemetry_cb = tele_append_all,
    };

    mqtt_configure(&mqtt_cfg);
    mqtt_init();
}

bool network_services_get_netif_ip(const char *if_key, char *buf, size_t buflen) {
    if (!buf || buflen == 0) {
        return false;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(if_key);
    esp_netif_ip_info_t ip_info;

    if (!netif || !esp_netif_is_netif_up(netif) ||
        esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        buf[0] = '\0';
        return false;
    }

    snprintf(buf, buflen, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

const char *network_services_get_hostname(void) {
    const char *hostname = config_get()->mdns_host;
    if (strlen(hostname) == 0) {
        hostname = config_get()->dev_name;
    }
    return hostname;
}

const char *network_services_get_device_url(void) {
    const char *hostname = network_services_get_hostname();
    snprintf(s_device_url, sizeof(s_device_url), "%s.local", hostname);
    return s_device_url;
}

void network_services_on_interval(supervisor_interval_stage_t stage) {
    if (stage == SUPERVISOR_INTERVAL_5S) {
        network_services_poll_internet_reachability();
    }
}

static void tele_common_mdns(const char *tele_id, cJSON *json_root) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s.local", network_services_get_hostname());
    cJSON_AddStringToObject(json_root, tele_id, buf);
}

void network_services_on_event(EventBits_t bits) {
    if (bits & SUPERVISOR_EVENT_PLATFORM_INITIALIZED) {
        tele_register("mdns", tele_common_mdns);
    }

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    if (bits & SUPERVISOR_EVENT_PLATFORM_INITIALIZED) {
        network_services_register_all_ha_entities();
        ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                                 .name = "IP",
                                                 .icon = "mdi:ip-outline",
                                                 .entity_category = "diagnostic"});
        ha_register_entity(&(ha_entity_config_t){
            .type = HA_SENSOR, .name = "mDNS", .icon = "mdi:dns", .entity_category = "diagnostic"});
    }
#endif

    if (bits & SUPERVISOR_EVENT_CMND_COMPLETED) {
        mqtt_trigger_telemetry();
    }
}

void network_services_mdns_init(void) {
    if (s_mdns_ready) {
        return; // already up — no-op, keeps existing hostname/instance intact
    }

#if CONFIG_NETWORK_UPLINK_THREAD && CONFIG_OPENTHREAD_SRP_CLIENT
    // Over Thread the name is registered with the border router (SRP), which announces it on the
    // LAN - mDNS has no interface to run on there
    thread_srp_host_init(network_services_get_hostname());
    if (NETWORK_HTTP_ENABLED) {
        bool secure = config_get()->http_secure;
        uint16_t port = secure ? config_get()->https_port : config_get()->http_port;
        thread_srp_add_service(network_services_get_hostname(),
                               secure ? "_https._tcp" : "_http._tcp", port);
    }
    s_mdns_ready = true;
#elif CONFIG_NETWORK_UPLINK_THREAD
    // mDNS has no interface to run on over Thread, so without an SRP client there is no name
    ESP_LOGW(TAG, "Thread without an SRP client: no name is announced");
#else
    const char *hostname = network_services_get_hostname();
    const char *instance = config_get()->mdns_instance;

    esp_err_t ret = (mdns_init() != ESP_OK || mdns_hostname_set(hostname) != ESP_OK ||
                     mdns_instance_name_set(instance) != ESP_OK)
                        ? ESP_FAIL
                        : ESP_OK;

    if (ret == ESP_OK) {
        s_mdns_ready = true;
        ESP_LOGI(TAG, "mDNS started with hostname: %s.local", hostname);
        if (NETWORK_HTTP_ENABLED) {
            bool secure = config_get()->http_secure;
            uint16_t port = secure ? config_get()->https_port : config_get()->http_port;
            mdns_service_add(NULL, secure ? "_https" : "_http", "_tcp", port, NULL, 0);
        }
    } else {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
    }
#endif
}

void network_services_mdns_shutdown(void) {
#if !CONFIG_NETWORK_UPLINK_THREAD
    mdns_free();
#endif
    s_mdns_ready = false;
}

void network_services_http_init(bool secure) {
    uint16_t port = secure ? config_get()->https_port : config_get()->http_port;
    http_init(&(http_config_t){
        .port = port,
        .ctrl_port = secure ? CONFIG_HTTPS_CTRL_PORT : CONFIG_HTTP_CTRL_PORT,
        .max_open_sockets = secure ? CONFIG_HTTPS_MAX_OPEN_SOCKETS : CONFIG_HTTP_MAX_OPEN_SOCKETS,
        .secure = secure,
    });
    http_register_json_get("/tele", tele_append_all);
    http_register_json_post("/cmnd", cmnd_process_json);
}

void network_services_http_start(void) { network_services_http_init(config_get()->http_secure); }

void network_services_ota_start(void) {
    tcp_ota_configure(config_get()->ota_port);
    tcp_ota_init();
}

void network_services_monitor_start(void) {
    tcp_monitor_configure(config_get()->monitor_port);
    tcp_monitor_init();
}

typedef struct {
    const char *name;
    void (*start)(void);
    void (*stop)(void);
} network_service_t;

static const network_service_t network_services[] = {
    {"mdns", network_services_mdns_init, network_services_mdns_shutdown},
    {"mqtt", network_services_mqtt_init, network_services_mqtt_shutdown},
    {"http", network_services_http_start, network_services_http_shutdown},
    {"sntp", network_services_sntp_init, network_services_sntp_shutdown},
    {"ota", network_services_ota_start, tcp_ota_shutdown},
    {"monitor", network_services_monitor_start, tcp_monitor_shutdown},
};

// {"service": {"ota": "on", "mdns": "off"}}
void network_services_service_handler(const char *args_json_str) {
    cJSON *args = json_str_as_object(args_json_str);
    if (!args) {
        ESP_LOGW(TAG, "Command aborted: invalid JSON arguments: %s", args_json_str);
        return;
    }

    for (const cJSON *item = args->child; item != NULL; item = item->next) {
        const network_service_t *service = NULL;
        for (size_t i = 0; i < sizeof(network_services) / sizeof(network_services[0]); i++) {
            if (strcmp(network_services[i].name, item->string) == 0) {
                service = &network_services[i];
                break;
            }
        }
        if (!service) {
            ESP_LOGW(TAG, "Unknown service: %s", item->string);
            continue;
        }

        logic_state_t state = json_as_logic_state(item);
        if (state == STATE_ON) {
            ESP_LOGI(TAG, "Starting %s", service->name);
            service->start();
        } else if (state == STATE_OFF) {
            ESP_LOGI(TAG, "Stopping %s", service->name);
            service->stop();
        }
    }

    cJSON_Delete(args);
}

void network_services_on_restart(void) {
    if (supervisor_is_safe_mode_active()) {
        // Skip graceful MQTT shutdown in safe mode to avoid delaying the restart
        // waiting on broker ack.
        return;
    }
    mqtt_publish_offline_state();
    network_services_mqtt_shutdown();
}

void network_services_sntp_init(void) {

    const char *servers[] = {config_get()->sntp_server1, config_get()->sntp_server2,
                             config_get()->sntp_server3};

    // Check if any server configured
    bool has_server = false;
    for (int i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; ++i) {
        if (servers[i] && strlen(servers[i]) > 0) {
            has_server = true;
            break;
        }
    }

    if (!has_server) {
        ESP_LOGW(TAG, "No SNTP servers configured, skipping SNTP init.");
        return;
    }

    esp_sntp_config_t config = {
        .smooth_sync = false,
        .server_from_dhcp = false,
        .wait_for_sync = true,
        .start = true,
        .sync_cb = sntp_sync_callback,
        .renew_servers_after_new_IP = false,
        .ip_event_to_renew = IP_EVENT_STA_GOT_IP,
        .index_of_first_server = 0,
        .num_of_servers = CONFIG_LWIP_SNTP_MAX_SERVERS,
        .servers = {servers[0], servers[1], servers[2]},
    };

    esp_netif_sntp_init(&config);
    ESP_LOGI(TAG, "SNTP service initialized, servers: %s, %s, %s",
             config.servers[0] ? config.servers[0] : "none",
             config.servers[1] ? config.servers[1] : "none",
             config.servers[2] ? config.servers[2] : "none");
}
