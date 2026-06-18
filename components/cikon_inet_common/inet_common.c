#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "mdns.h"

#include "bits_helper.h"
#include "cmnd.h"
#include "config_manager.h"
#include "https_server.h"
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
#include "inet_common.h"
#include "tcp_monitor.h"
#include "tcp_ota.h"

#define TAG "cikon:inet_common"

static char s_device_url[64];

static void sntp_sync_callback(struct timeval *tv) {
    char time_str[32];
    format_current_time(time_str, sizeof(time_str), NULL);
    ESP_LOGW(TAG, "SNTP time synchronized: %s", time_str);
}

void inet_common_mqtt_init(void) {
    const char *device_url = inet_common_get_device_url();
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

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY

#if CONFIG_SUPERVISOR_TELE_TASKS
static void build_tasks_dict_ha(cJSON *payload, const char *sanitized_name) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{{ value_json.%s | count }}", sanitized_name);
    cJSON_ReplaceItemInObject(payload, "val_tpl", cJSON_CreateString(buf));
    snprintf(buf, sizeof(buf), "{{ value_json.%s | tojson }}", sanitized_name);
    cJSON_AddStringToObject(payload, "json_attr_tpl", buf);
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
}
#endif

void inet_common_register_all_ha_entities(void) {
    ESP_LOGI(TAG, "Registering all HA entities from adapters");

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

#if CONFIG_SUPERVISOR_TELE_TASKS
    ha_register_entity(&(ha_entity_config_t){
        .type = HA_SENSOR,
        .name = "Tasks Dict",
        .entity_category = "diagnostic",
        .custom_builder = build_tasks_dict_ha,
    });
#endif
}

void inet_common_ha_discovery_handler(const char *args_json_str) {
    logic_state_t force_empty_payload = json_str_as_logic_state(args_json_str);
    if (force_empty_payload == STATE_TOGGLE) {
        ESP_LOGE(TAG, "Toggling is not permitted for HA discovery");
        return;
    }

    ESP_LOGI(TAG, "Triggering Home Assistant MQTT discovery");
    publish_ha_mqtt_discovery(force_empty_payload == STATE_OFF);
}
#endif

bool get_netif_ip(const char *if_key, char *buf, size_t buflen) {
    if (!buf || buflen == 0) {
        return false;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(if_key);
    esp_netif_ip_info_t ip_info;

    if (!netif || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        buf[0] = '\0';
        return false;
    }

    snprintf(buf, buflen, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

const char *inet_common_get_hostname(void) {
    const char *hostname = config_get()->mdns_host;
    if (strlen(hostname) == 0) {
        hostname = config_get()->dev_name;
    }
    return hostname;
}

const char *inet_common_get_device_url(void) {
    const char *hostname = inet_common_get_hostname();
    snprintf(s_device_url, sizeof(s_device_url), "%s.local", hostname);
    return s_device_url;
}

bool is_internet_reachable(void) { return is_tcp_port_reachable("8.8.8.8", 53); }

void inet_common_poll_internet_reachability(void) {

    static bool last_internet_reachable = false;

    bool current_state = is_internet_reachable();
    if (current_state != last_internet_reachable) {
        supervisor_notify_event(current_state ? INET_INTERNET_READY : INET_INTERNET_LOST);
        last_internet_reachable = current_state;
    }
}

bool is_tcp_port_reachable(const char *host, uint16_t port) {
    struct sockaddr_in addr;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // Resolve the host name may take time !. For real non blocking connect use ip address.
    if (inet_aton(host, &addr.sin_addr) == 0) {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            close(sock);
            return false;
        }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int res = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (res == 0) {
        close(sock);
        return true;
    } else if (errno != EINPROGRESS) {
        close(sock);
        return false;
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = 500 * 1000};
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    res = select(sock + 1, NULL, &wfds, NULL, &tv);
    if (res > 0) {
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
        close(sock);
        return so_error == 0;
    } else {
        close(sock);
        return false;
    }
}

void inet_common_on_interval(supervisor_interval_stage_t stage) {
    if (stage == SUPERVISOR_INTERVAL_5S) {
        inet_common_poll_internet_reachability();
    }
}

void inet_common_on_event(EventBits_t bits) {
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    if (bits & SUPERVISOR_EVENT_PLATFORM_INITIALIZED) {
        inet_common_register_all_ha_entities();
        ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                                 .name = "IP",
                                                 .icon = "mdi:ip-outline",
                                                 .entity_category = "diagnostic"});
    }
#endif

    if (bits & SUPERVISOR_EVENT_CMND_COMPLETED) {
        mqtt_trigger_telemetry();
    }
}

void inet_common_mdns_init(void) {
    const char *hostname = inet_common_get_hostname();
    const char *instance = config_get()->mdns_instance;

    esp_err_t ret = (mdns_init() != ESP_OK || mdns_hostname_set(hostname) != ESP_OK ||
                     mdns_instance_name_set(instance) != ESP_OK)
                        ? ESP_FAIL
                        : ESP_OK;

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "mDNS started with hostname: %s.local", hostname);
    } else {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
    }
}

void inet_common_mdns_shutdown(void) { mdns_free(); }

void inet_common_sntp_handler(const char *args_json_str) {
    logic_state_t state = json_str_as_logic_state(args_json_str);
    if (state == STATE_ON) {
        ESP_LOGI(TAG, "Starting SNTP service");
        inet_common_sntp_init();
    } else if (state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping SNTP service");
        inet_common_sntp_shutdown();
    } else {
        ESP_LOGW(TAG, "Invalid SNTP state");
    }
}

void inet_common_ota_handler(const char *args_json_str) {
    logic_state_t state = json_str_as_logic_state(args_json_str);
    if (state == STATE_ON) {
        ESP_LOGI(TAG, "Starting OTA update");
        tcp_ota_init();
    } else if (state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping OTA update");
        tcp_ota_shutdown();
    }
}

void inet_common_monitor_handler(const char *args_json_str) {
    logic_state_t state = json_str_as_logic_state(args_json_str);
    if (state == STATE_ON) {
        ESP_LOGI(TAG, "Starting TCP monitor");
        tcp_monitor_init();
    } else if (state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping TCP monitor");
        tcp_monitor_shutdown();
    }
}

void inet_common_http_init(void) {
    http_init();
    http_register_json_get("/tele", tele_append_all);
    http_register_json_post("/cmnd", cmnd_process_json);
}

void inet_common_http_handler(const char *args_json_str) {
    logic_state_t state = json_str_as_logic_state(args_json_str);
    if (state == STATE_ON) {
        ESP_LOGI(TAG, "Starting HTTP server");
        inet_common_http_init();
    } else if (state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping HTTP server");
        http_shutdown();
    }
}

void inet_common_https_init(void) {
    static const https_endpoint_config_t endpoints[] = {
        {.uri = "/cmnd", .method = HTTP_POST, .json_cmnd = cmnd_process_json},
        {.uri = "/tele", .method = HTTP_GET, .json_tele = tele_append_all},
        {.uri = NULL}};
    https_configure(endpoints, config_get()->http_auth);
    https_init();
}

void inet_common_https_handler(const char *args_json_str) {
    logic_state_t state = json_str_as_logic_state(args_json_str);
    if (state == STATE_ON) {
        ESP_LOGI(TAG, "Starting HTTPS server");
        inet_common_https_init();
    } else if (state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping HTTPS server");
        https_shutdown();
    } else {
        ESP_LOGW(TAG, "Invalid HTTPS state");
    }
}

void inet_common_sntp_init(void) {

    const char *servers[] = {config_get()->sntp1, config_get()->sntp2, config_get()->sntp3};

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
