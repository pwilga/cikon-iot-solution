#include "cJSON.h"
#include "esp_eth.h"   // IWYU pragma: keep
#include "esp_event.h" // IWYU pragma: keep
#include "esp_log.h"
#include "esp_netif.h"

#include "bits_helper.h"
#include "ethernet.h"
#include "inet_common.h"
#include "supervisor.h"
#include "tele.h"
#include "uplink.h"

#define TAG "cikon:network:ethernet"

static esp_event_handler_instance_t eth_event_instance = NULL;
static esp_event_handler_instance_t ip_event_instance = NULL;

static void uplink_ethernet_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                          void *event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Ethernet Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        supervisor_notify_event(NETWORK_UPLINK_READY);
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
            supervisor_notify_event(NETWORK_UPLINK_LOST);
            break;
        }
    }
}

static void uplink_ethernet_unregister_handlers(void) {

    if (ip_event_instance) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_event_instance);
        ip_event_instance = NULL;
    }

    if (eth_event_instance) {
        esp_event_handler_instance_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_instance);
        eth_event_instance = NULL;
    }
}

static esp_err_t uplink_ethernet_start(void) {

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, &uplink_ethernet_event_handler, NULL, &eth_event_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, &uplink_ethernet_event_handler, NULL, &ip_event_instance));

    esp_err_t ret = ethernet_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet stack init failed: %s", esp_err_to_name(ret));
        uplink_ethernet_unregister_handlers();
        return ret;
    }

    return ESP_OK;
}

static esp_err_t uplink_ethernet_stop(void) {
    uplink_ethernet_unregister_handlers();
    return ethernet_shutdown();
}

static void uplink_ethernet_tele_ip_address(const char *tele_id, cJSON *json_root) {
    char ip[16];
    get_netif_ip("ETH_DEF", ip, sizeof(ip));
    cJSON_AddStringToObject(json_root, tele_id, ip);
}

static const tele_entry_t uplink_ethernet_tele[] = {{"ip", uplink_ethernet_tele_ip_address},
                                                    {NULL, NULL}};

const uplink_t uplink_ethernet = {
    .name = "ethernet",
    .start = uplink_ethernet_start,
    .stop = uplink_ethernet_stop,
    .commands = NULL,
    .tele = uplink_ethernet_tele,
};
