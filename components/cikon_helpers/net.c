#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
// #include "platform_services.h"
// #include "config_manager.h"
#include "esp_log.h"
#include "net.h"
#include "mdns.h"

#define TAG "cikon:helpers:net"

static const char *mdns_host = NULL;
static const char *mdns_instance = NULL;

static const char *sntp_servers[CONFIG_LWIP_SNTP_MAX_SERVERS] = {NULL};
static esp_sntp_time_cb_t sntp_cb = NULL;

// bool is_network_connected(void) {
//     return xEventGroupGetBits(app_event_group) & (WIFI_STA_CONNECTED_BIT | WIFI_AP_STARTED_BIT);
// }

bool is_internet_reachable(void) { return is_tcp_port_reachable("8.8.8.8", 53); }

bool is_tcp_port_reachable(const char *host, uint16_t port) {

    // if (!(xEventGroupGetBits(app_event_group) & WIFI_STA_CONNECTED_BIT)) {
    //     return false;
    // }

    struct sockaddr_in addr;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return false;

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

// Helper: parse URI like mqtts://host:port
static bool parse_mqtt_broker_uri(const char *uri, char *host, size_t host_len, uint16_t *port) {
    // Find "://"
    const char *p = strstr(uri, "://");
    if (!p)
        return false;
    p += 3; // skip scheme
    const char *colon = strrchr(p, ':');
    const char *slash = strchr(p, '/');
    if (!colon || (slash && colon > slash))
        return false;
    size_t len = colon - p;
    if (len >= host_len)
        return false;
    strncpy(host, p, len);
    host[len] = '\0';
    *port = (uint16_t)atoi(colon + 1);
    return true;
}

// bool is_mqtt_broker_reachable(void) {
//     char host[128];
//     uint16_t port;
//     const char *uri = config_get()->mqtt_broker;
//     if (!parse_mqtt_broker_uri(uri, host, sizeof(host), &port)) {
//         return false;
//     }
//     return is_tcp_port_reachable(host, port);
// }

void net_mdns_configure(const char *hostname, const char *instance_name) {
    mdns_host = hostname;
    mdns_instance = instance_name;
}

void net_mdns_init(void) {

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

void net_mdns_shutdown(void) {
    mdns_free();
}

void net_sntp_configure(const char **servers, esp_sntp_time_cb_t cb) {
    for (int i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; ++i) {
        if (servers[i] && strlen(servers[i]) > 0) {
            sntp_servers[i] = servers[i];
        } else {
            sntp_servers[i] = NULL;
        }
    }
    sntp_cb = cb;
}

void net_sntp_init(void) {
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
