#include <string.h>
#include <sys/time.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "esp_timer.h"

#include "platform_services.h"

#define TAG "cikon-systems"

static const char *mdns_host = NULL;
static const char *mdns_instance = NULL;

static const char *sntp_servers[CONFIG_LWIP_SNTP_MAX_SERVERS] = {NULL};
static esp_sntp_time_cb_t sntp_cb = NULL;

static void (*restart_callback)(void) = NULL;

static bool onboard_led_state = true;

void core_system_init(void) {
    // onbard_led
    ESP_ERROR_CHECK(gpio_reset_pin(CONFIG_BOARD_STATUS_LED_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(CONFIG_BOARD_STATUS_LED_GPIO, GPIO_MODE_OUTPUT));
}

void set_restart_callback(void (*cb)(void)) { restart_callback = cb; }

void esp_safe_restart() {

    if (restart_callback) {
        restart_callback();
    }

    esp_restart();
}

void mdns_service_configure(const char *hostname, const char *instance_name) {
    mdns_host = hostname;
    mdns_instance = instance_name;
}

void mdns_service_init() {

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

void sntp_service_configure(const char **servers, esp_sntp_time_cb_t cb) {

    for (int i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; ++i) {
        if (servers[i] && strlen(servers[i]) > 0) {
            sntp_servers[i] = servers[i];
        } else {
            sntp_servers[i] = NULL;
        }
    }
    sntp_cb = cb;
}

void sntp_service_init() {

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
    ESP_LOGI(TAG, "SNTP service initialized, current servers: %s, %s, %s",
             config.servers[0] ? config.servers[0] : "none",
             config.servers[1] ? config.servers[1] : "none",
             config.servers[2] ? config.servers[2] : "none");
}

const char *get_client_id(void) {

    static char buf[13] = {0};
    static bool initialized = false;

    if (initialized)
        return buf;

    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK)
        return NULL;

    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", MAC2STR(mac));

    initialized = true;
    return buf;
}

const char *get_boot_time(void) {

    static char iso8601[32] = {0};
    static bool initialized = false;

    if (!initialized) {
        struct timeval now;
        gettimeofday(&now, NULL);

        int64_t uptime_us = esp_timer_get_time();
        time_t boot_time = now.tv_sec - (uptime_us / 1000000);

        strftime(iso8601, sizeof(iso8601), "%Y-%m-%dT%H:%M:%SZ", gmtime(&boot_time));
        initialized = true;
    }

    return iso8601;
}

bool get_onboard_led_state(void) { return onboard_led_state; }

void onboard_led_set_state(bool state) {

    if (onboard_led_state != state) {
        ESP_ERROR_CHECK(gpio_set_level(CONFIG_BOARD_STATUS_LED_GPIO, !state));
        onboard_led_state = state;
    }
}

esp_err_t nvs_flash_safe_init() {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

void reset_nvs_partition(void) {
    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS ERASE: All keys erased successfully.");
        ESP_ERROR_CHECK(nvs_flash_safe_init());
    } else {
        ESP_LOGE(TAG, "NVS ERASE: Failed to erase NVS: %s", esp_err_to_name(err));
    }
}
