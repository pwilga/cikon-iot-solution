#include "mesh_lite.h"
#include "esp_bridge.h"
#include "esp_log.h"
#include "esp_mesh_lite.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include <string.h>

#define TAG "cikon:mesh_lite"

static bool initialized = false;
static mesh_lite_config_t config = {0};

void mesh_lite_configure(const mesh_lite_config_t *cfg) {
    if (cfg) {
        config = *cfg;
        ESP_LOGI(TAG, "Configured mesh: mesh_id=%d, ap_ssid=%s", cfg->mesh_id, cfg->ap_ssid);
    }
}

esp_err_t mesh_lite_init(void) {

    if (initialized) {
        ESP_LOGW(TAG, "Already initialized (mesh_id=0x%02X)", config.mesh_id);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    ESP_LOGI(TAG, "Initializing ESP-MESH Lite (mesh_id=0x%02X)", config.mesh_id);

    esp_bridge_create_all_netif();

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, config.sta_ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, config.sta_password, sizeof(sta_config.sta.password));
    esp_bridge_wifi_set_config(WIFI_IF_STA, &sta_config);

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, config.ap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, config.ap_password, sizeof(ap_config.ap.password));
    esp_bridge_wifi_set_config(WIFI_IF_AP, &ap_config);

    esp_mesh_lite_config_t esp_config = ESP_MESH_LITE_DEFAULT_INIT();

    esp_config.mesh_id = config.mesh_id;
    esp_config.softap_ssid = config.ap_ssid;
    esp_config.softap_password = config.ap_password;

    esp_mesh_lite_init(&esp_config);
    esp_mesh_lite_start();

    initialized = true;
    ESP_LOGI(TAG, "ESP-MESH Lite initialized successfully");
    return ESP_OK;
}

esp_err_t mesh_lite_shutdown(void) {
    if (!initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Shutting down ESP-MESH Lite");

    esp_err_t ret = esp_mesh_lite_disconnect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disconnect from mesh: %d", ret);
    }

    initialized = false;
    ESP_LOGI(TAG, "ESP-MESH Lite shutdown complete");
    return ESP_OK;
}

bool is_mesh_root_node(void) {
    if (!initialized) {
        return false;
    }

    return esp_mesh_lite_get_level() == 1;
}

int mesh_lite_get_child_count(void) {
    if (!initialized) {
        return 0;
    }

    wifi_sta_list_t sta_list = {0};
    esp_err_t ret = esp_wifi_ap_get_sta_list(&sta_list);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get STA list: %d", ret);
        return 0;
    }

    return sta_list.num;
}

void mesh_lite_get_node_ip(char *buf, size_t buflen) {
    if (!buf || buflen == 0) {
        return;
    }

    if (!initialized) {
        buf[0] = '\0';
        return;
    }

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta_netif) {
        snprintf(buf, buflen, "0.0.0.0");
        return;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(sta_netif, &ip_info);
    if (ret != ESP_OK) {
        snprintf(buf, buflen, "0.0.0.0");
        return;
    }

    snprintf(buf, buflen, IPSTR, IP2STR(&ip_info.ip));
}
