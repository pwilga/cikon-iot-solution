#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "mdns.h"

#include "cmnd.h"
#include "config_manager.h"
#include "json_parser.h"
#include "mqtt.h"
#include "platform_services.h"
#include "supervisor.h"
#include "tele.h"
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
#include "ha.h"
#include "metadata.h"
#endif

#include "inet_common.h"

#define TAG "cikon:inet_common"

static char s_device_url[64];

// mDNS configuration
static const char *mdns_host = NULL;
static const char *mdns_instance = NULL;

// SNTP configuration
static const char *sntp_servers[CONFIG_LWIP_SNTP_MAX_SERVERS] = {NULL};
static esp_sntp_time_cb_t sntp_cb = NULL;

void inet_common_configure_mqtt(void) {
    const char *device_url = inet_common_get_device_url();

    mqtt_config_t mqtt_cfg = {
        .client_id = get_client_id(),
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
        .telemetry_cb = tele_append_all,
    };

    mqtt_configure(&mqtt_cfg);
    ESP_LOGI(TAG, "MQTT configured (broker: %s, node: %s)", mqtt_cfg.mqtt_broker,
             mqtt_cfg.mqtt_node);
}

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
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

void inet_common_mdns_configure(const char *hostname, const char *instance_name) {
    mdns_host = hostname;
    mdns_instance = instance_name;
}

void inet_common_mdns_init(void) {
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

void inet_common_mdns_shutdown(void) { mdns_free(); }

void inet_common_sntp_configure(const char **servers, esp_sntp_time_cb_t cb) {
    for (int i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; ++i) {
        if (servers[i] && strlen(servers[i]) > 0) {
            sntp_servers[i] = servers[i];
        } else {
            sntp_servers[i] = NULL;
        }
    }
    sntp_cb = cb;
}

void inet_common_sntp_init(void) {
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
