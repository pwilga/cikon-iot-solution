#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "mdns.h"

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
#include "inet_common.h"
#include "tcp_monitor.h"
#include "tcp_ota.h"

#if CONFIG_OPENTHREAD_ENABLED
#include "esp_openthread.h"
#include "esp_openthread_dns64.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "openthread/srp_client.h"
#include "openthread/thread.h"
#endif

#define TAG "cikon:inet_common"

static char s_device_url[64];
static bool s_mdns_ready = false;

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

static void build_chip_attrs(cJSON *payload, const char *sanitized_name) {
    (void)sanitized_name;
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
    cJSON_AddStringToObject(payload, "json_attr_tpl",
                            "{{ {'features': value_json.features, 'cores': value_json.cores, "
                            "'chip_rev': value_json.chip_rev, 'cpu_freq': value_json.cpu_freq, "
                            "'flash_size': value_json.flash_size, "
                            "'psram_size': value_json.psram_size | default(0)} "
                            "| tojson }}");
}

static void build_fs_attrs(cJSON *payload, const char *sanitized_name) {
    (void)sanitized_name;
    cJSON_ReplaceItemInObject(
        payload, "val_tpl",
        cJSON_CreateString("{{ (100 * value_json.fs_used / value_json.fs_size) | round(1) }}"));
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
    cJSON_AddStringToObject(payload, "json_attr_tpl",
                            "{{ {'fs_used': value_json.fs_used, "
                            "'fs_size': value_json.fs_size} | tojson }}");
}

static void build_dram_attrs(cJSON *payload, const char *sanitized_name) {
    (void)sanitized_name;
    cJSON_ReplaceItemInObject(
        payload, "val_tpl",
        cJSON_CreateString(
            "{{ (100 * (1 - value_json.dram_free / value_json.dram_size)) | round(1) }}"));
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
    cJSON_AddStringToObject(payload, "json_attr_tpl",
                            "{{ {'dram_free': value_json.dram_free, "
                            "'dram_min_free': value_json.dram_min_free, "
                            "'dram_size': value_json.dram_size} | tojson }}");
}

static void build_psram_attrs(cJSON *payload, const char *sanitized_name) {
    (void)sanitized_name;
    cJSON_ReplaceItemInObject(
        payload, "val_tpl",
        cJSON_CreateString(
            "{{ (100 * (1 - value_json.psram_free / value_json.psram_size)) | round(1) }}"));
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
    cJSON_AddStringToObject(payload, "json_attr_tpl",
                            "{{ {'psram_free': value_json.psram_free, "
                            "'psram_min_free': value_json.psram_min_free, "
                            "'psram_size': value_json.psram_size} | tojson }}");
}

static void build_app_build_time_attrs(cJSON *payload, const char *sanitized_name) {
    (void)sanitized_name;
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
    cJSON_AddStringToObject(payload, "json_attr_tpl",
                            "{{ {'version': value_json.version, 'idf': value_json.idf} "
                            "| tojson }}");
}

static void build_bootloader_build_time_attrs(cJSON *payload, const char *sanitized_name) {
    (void)sanitized_name;
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
    cJSON_AddStringToObject(payload, "json_attr_tpl",
                            "{{ {'version': value_json.bootloader_version, "
                            "'idf': value_json.bootloader_idf} | tojson }}");
}

static void register_core_ha_entities(void) {
    // Runtime
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "Boot Time",
                                             .device_class = "timestamp",
                                             .icon = "mdi:clock-start",
                                             .entity_category = "diagnostic"});
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "DRAM",
                                             .unit = "%",
                                             .icon = "mdi:memory",
                                             .entity_category = "diagnostic",
                                             .custom_builder = build_dram_attrs});
    if (get_psram_size() > 0) {
        ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                                 .name = "PSRAM",
                                                 .unit = "%",
                                                 .icon = "mdi:memory",
                                                 .entity_category = "diagnostic",
                                                 .custom_builder = build_psram_attrs});
    }

    // Device info
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "Chip",
                                             .icon = "mdi:chip",
                                             .entity_category = "diagnostic",
                                             .custom_builder = build_chip_attrs});
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "ID",
                                             .icon = "mdi:identifier",
                                             .entity_category = "diagnostic"});
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "App Build Time",
                                             .device_class = "timestamp",
                                             .icon = "mdi:wrench-clock",
                                             .entity_category = "diagnostic",
                                             .custom_builder = build_app_build_time_attrs});

    // Bootloader
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "Bootloader Build Time",
                                             .device_class = "timestamp",
                                             .icon = "mdi:clock-outline",
                                             .entity_category = "diagnostic",
                                             .custom_builder = build_bootloader_build_time_attrs});

    // OTA
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "OTA State",
                                             .icon = "mdi:cloud-upload-outline",
                                             .entity_category = "diagnostic"});

    // Hardware
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "Reset Reason",
                                             .icon = "mdi:restart-alert",
                                             .entity_category = "diagnostic"});
    ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                             .name = "FS",
                                             .unit = "%",
                                             .icon = "mdi:folder-outline",
                                             .entity_category = "diagnostic",
                                             .custom_builder = build_fs_attrs});
    float chip_temp;
    if (get_chip_temp(&chip_temp)) {
        ha_register_entity(&(ha_entity_config_t){.type = HA_SENSOR,
                                                 .name = "Chip Temp",
                                                 .device_class = "temperature",
                                                 .unit = "°C",
                                                 .icon = "mdi:thermometer",
                                                 .entity_category = "diagnostic"});
    }

    // Controls
    ha_register_entity(&(ha_entity_config_t){.type = HA_BUTTON,
                                             .name = "Restart",
                                             .device_class = "restart",
                                             .icon = "mdi:restart",
                                             .entity_category = "diagnostic"});
}

void inet_common_register_all_ha_entities(void) {
    ESP_LOGI(TAG, "Registering all HA entities from adapters");

    register_core_ha_entities();

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

    if (!netif || !esp_netif_is_netif_up(netif) ||
        esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
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

#if CONFIG_OPENTHREAD_SRP_CLIENT || CONFIG_OPENTHREAD_DNS64_CLIENT
// Thread is the way out (Thread device, not a border router) - the same check IDF's DNS64 hook uses
static bool thread_is_default_netif(void) {
    esp_netif_t *thread_netif = esp_openthread_get_netif();
    return thread_netif && esp_netif_get_default_netif() == thread_netif;
}
#endif

#if CONFIG_OPENTHREAD_SRP_CLIENT
static otSrpClientService s_srp_http_service;

static void srp_client_callback(otError error, const otSrpClientHostInfo *hostInfo,
                                const otSrpClientService *services,
                                const otSrpClientService *removedServices, void *context) {
    (void)hostInfo;
    (void)services;
    (void)removedServices;
    (void)context;
    if (error == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "SRP: registered OK");
    } else if (error == OT_ERROR_DUPLICATED) {
        // SRP key mismatch — NVS was likely erased. Remove the stale host entry on the OBR.
        // To clear all SRP registrations on OBR: ot srp server disable && ot srp server enable
        ESP_LOGE(TAG, "SRP: name conflict for %s — remove stale host on OBR",
                 inet_common_get_hostname());
    } else {
        ESP_LOGW(TAG, "SRP error: %s", otThreadErrorToString(error));
    }
}

static void srp_host_init(void) {
    const char *hostname = inet_common_get_hostname();
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    otSrpClientSetCallback(instance, srp_client_callback, NULL);
    otError err = otSrpClientSetHostName(instance, hostname);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "SRP set hostname: %s", otThreadErrorToString(err));
        esp_openthread_lock_release();
        return;
    }
    err = otSrpClientEnableAutoHostAddress(instance);
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "SRP auto host addr: %s", otThreadErrorToString(err));
    }
    otSrpClientEnableAutoStartMode(instance, NULL, NULL);
    ESP_LOGI(TAG, "SRP host: %s, lease: %lus", hostname,
             (unsigned long)otSrpClientGetLeaseInterval(instance));
    esp_openthread_lock_release();
}

static void srp_add_service(otSrpClientService *svc, const char *type, uint16_t port) {
    memset(svc, 0, sizeof(*svc));
    svc->mName = type;
    svc->mInstanceName = inet_common_get_hostname();
    svc->mPort = port;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = otSrpClientAddService(esp_openthread_get_instance(), svc);
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
        ESP_LOGE(TAG, "SRP add %s: %s", type, otThreadErrorToString(err));
    } else {
        ESP_LOGI(TAG, "SRP: %s port %d", type, port);
    }
}
#endif

bool is_internet_reachable(void) {
#if CONFIG_OPENTHREAD_DNS64_CLIENT
    // Same as `ot ping 8.8.8.8`: Thread has no IPv4, the BR's NAT64 prefix gets glued to the
    // address
    if (thread_is_default_netif()) {
        ip6_addr_t nat64;
        if (esp_openthread_get_nat64_prefix(&nat64) != ESP_OK) {
            return false;
        }
        nat64.addr[3] = ipaddr_addr("8.8.8.8");

        char nat64_address[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(&nat64, nat64_address, sizeof(nat64_address));
        return is_tcp_port_reachable(nat64_address, 53);
    }
#endif
    return is_tcp_port_reachable("8.8.8.8", 53);
}

void inet_common_poll_internet_reachability(void) {

    static bool last_internet_reachable = false;

    bool current_state = is_internet_reachable();
    if (current_state != last_internet_reachable) {
        supervisor_notify_event(current_state ? NETWORK_INTERNET_READY : NETWORK_INTERNET_LOST);
        last_internet_reachable = current_state;
    }
}

bool is_tcp_port_reachable(const char *host, uint16_t port) {
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));

    struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
    struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
    socklen_t addr_len;

    // Resolve the host name may take time !. For real non blocking connect use ip address.
    if (inet_aton(host, &addr4->sin_addr) != 0) {
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port);
        addr_len = sizeof(*addr4);
    } else if (inet_pton(AF_INET6, host, &addr6->sin6_addr) == 1) {
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(port);
        addr_len = sizeof(*addr6);
    } else {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            return false;
        }
        memcpy(&addr4->sin_addr, he->h_addr, he->h_length);
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port);
        addr_len = sizeof(*addr4);
    }

    int sock = socket(addr.ss_family, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int res = connect(sock, (struct sockaddr *)&addr, addr_len);
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

static void tele_common_mdns(const char *tele_id, cJSON *json_root) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s.local", inet_common_get_hostname());
    cJSON_AddStringToObject(json_root, tele_id, buf);
}

void inet_common_on_event(EventBits_t bits) {
    if (bits & SUPERVISOR_EVENT_PLATFORM_INITIALIZED) {
        tele_register("mdns", tele_common_mdns);
    }

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    if (bits & SUPERVISOR_EVENT_PLATFORM_INITIALIZED) {
        inet_common_register_all_ha_entities();
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

void inet_common_mdns_init(void) {
    if (s_mdns_ready) {
        return; // already up — no-op, keeps existing hostname/instance intact
    }

#if CONFIG_OPENTHREAD_SRP_CLIENT
    // Over Thread the name is registered with the border router (SRP), which announces it on the
    // LAN - mDNS has no interface to run on there
    if (thread_is_default_netif()) {
        srp_host_init();
        bool secure = config_get()->http_secure;
        uint16_t port = secure ? config_get()->https_port : config_get()->http_port;
        srp_add_service(&s_srp_http_service, secure ? "_https._tcp" : "_http._tcp", port);
        s_mdns_ready = true;
        return;
    }
#endif

    const char *hostname = inet_common_get_hostname();
    const char *instance = config_get()->mdns_instance;

    esp_err_t ret = (mdns_init() != ESP_OK || mdns_hostname_set(hostname) != ESP_OK ||
                     mdns_instance_name_set(instance) != ESP_OK)
                        ? ESP_FAIL
                        : ESP_OK;

    if (ret == ESP_OK) {
        s_mdns_ready = true;
        ESP_LOGI(TAG, "mDNS started with hostname: %s.local", hostname);
    } else {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
    }
}

void inet_common_mdns_shutdown(void) {
    mdns_free();
    s_mdns_ready = false;
}

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

void inet_common_http_init(bool secure) {
    uint16_t port = secure ? config_get()->https_port : config_get()->http_port;
    http_init(&(http_config_t){
        .port = port,
        .ctrl_port = secure ? CONFIG_HTTPS_CTRL_PORT : CONFIG_HTTP_CTRL_PORT,
        .max_open_sockets = secure ? CONFIG_HTTPS_MAX_OPEN_SOCKETS : CONFIG_HTTP_MAX_OPEN_SOCKETS,
        .secure = secure,
    });
    http_register_json_get("/tele", tele_append_all);
    http_register_json_post("/cmnd", cmnd_process_json);
    if (s_mdns_ready) {
        mdns_service_add(NULL, secure ? "_https" : "_http", "_tcp", port, NULL, 0);
    }
}

void inet_common_http_handler(const char *args_json_str) {
    logic_state_t state = json_str_as_logic_state(args_json_str);
    if (state == STATE_ON) {
        ESP_LOGI(TAG, "Starting HTTP server");
        inet_common_http_init(config_get()->http_secure);
    } else if (state == STATE_OFF) {
        ESP_LOGI(TAG, "Stopping HTTP server");
        http_shutdown();
    }
}

void inet_common_on_restart(void) {
    if (supervisor_is_safe_mode_active()) {
        // Skip graceful MQTT shutdown in safe mode to avoid delaying the restart
        // waiting on broker ack.
        return;
    }
    mqtt_publish_offline_state();
    inet_common_mqtt_shutdown();
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
