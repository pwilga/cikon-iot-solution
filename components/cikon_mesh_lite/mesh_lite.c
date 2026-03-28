#include "mesh_lite.h"
#include "esp_bridge.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh_lite.h"
#include "esp_mesh_lite_core.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include <string.h>

#define TAG "cikon:mesh_lite"

static bool initialized = false;
static mesh_lite_config_t config = {0};
static mesh_message_callback_t message_callback = NULL;

// Forward declaration
static cJSON *mesh_lite_message_handler(cJSON *payload, uint32_t seq);

static const esp_mesh_lite_msg_action_t msg_actions[] = {
    {"message", NULL, mesh_lite_message_handler}, {NULL, NULL, NULL} /* Must be NULL terminated */
};

// Internal mesh message handler
static cJSON *mesh_lite_message_handler(cJSON *payload, uint32_t seq) {

    // ESP_LOGI(TAG, "Received mesh message");
    if (!payload) {
        ESP_LOGW(TAG, "Payload is NULL");
        return NULL;
    }

    // Log entire payload with seq
    char *payload_str = cJSON_PrintUnformatted(payload);
    if (payload_str) {
        ESP_LOGI(TAG, "Received payload (seq=%lu): %s", (unsigned long)seq, payload_str);
        free(payload_str);
    }

    const char *target = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "target"));

    // Check if message is for us (broadcast or matches our name)
    bool for_me = false;
    if (!target) {
        for_me = true;
    } else if (strcmp(target, "broadcast") == 0 || strcmp(target, "all") == 0) {
        for_me = true;
    } else if (config.device_name && strcmp(target, config.device_name) == 0) {
        for_me = true;
    }

    // Process message if it's for us
    if (for_me && message_callback) {
        message_callback(payload);
    }

    // If root, always broadcast to children (relay) using proper API
    if (is_mesh_root_node()) {
        ESP_LOGI(TAG, "Root forwarding message to children");

        esp_mesh_lite_msg_config_t forward_config = {
            .json_msg = {.send_msg = "message",
                         .expect_msg = NULL,
                         .max_retry = 0,
                         .retry_interval = 1000,
                         .req_payload = payload, // Forward same payload
                         .resend = &esp_mesh_lite_send_broadcast_msg_to_child,
                         .send_fail = NULL}};

        esp_mesh_lite_send_msg(ESP_MESH_LITE_JSON_MSG, &forward_config);
    }

    return NULL;
}

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

    esp_config.join_mesh_ignore_router_status = true;
    esp_config.join_mesh_without_configured_wifi = true;

    esp_mesh_lite_init(&esp_config);

    // Register mesh message handler BEFORE start (like official examples)
    esp_err_t ret = esp_mesh_lite_msg_action_list_register(msg_actions);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register message handler: %d", ret);
    } else {
        ESP_LOGI(TAG, "Registered mesh message handler");
    }

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

void mesh_log_topology(void) {
    if (!initialized) {
        return;
    }

    uint8_t level = esp_mesh_lite_get_level();
    const char *role = (level == 1) ? "ROOT" : "CHILD";
    uint8_t mesh_id = esp_mesh_lite_get_mesh_id();
    const char *leaf_marker = "";

    if (esp_mesh_lite_is_leaf_node()) {
        leaf_marker = " [L]";
    }

    ESP_LOGI(TAG, "Mesh role: %s%s (Level %d), mesh_id: 0x%02X", role, leaf_marker, level, mesh_id);

#ifdef CONFIG_MESH_LITE_NODE_INFO_REPORT
    uint32_t node_count = esp_mesh_lite_get_mesh_node_number();
    ESP_LOGI(TAG, "Total nodes in mesh: %lu", (unsigned long)node_count);

    uint32_t list_size = 0;
    const node_info_list_t *nodes = esp_mesh_lite_get_nodes_list(&list_size);

    if (nodes && list_size > 0) {
        const node_info_list_t *current = nodes;
        while (current) {
            esp_ip4_addr_t ip_addr = {.addr = current->node->ip_addr};
            const char *marker = (current->node->level == 1) ? " [R]" : "";
            ESP_LOGI(TAG, "  MAC: " MACSTR ", Level: %d, IP: " IPSTR "%s",
                     MAC2STR(current->node->mac_addr), current->node->level, IP2STR(&ip_addr),
                     marker);
            current = current->next;
        }
    }
#endif
}

void mesh_lite_register_message_callback(mesh_message_callback_t callback) {
    message_callback = callback;
    // ESP_LOGI(TAG, "Message callback %s", callback ? "registered" : "unregistered");
}

esp_err_t mesh_lite_send_message(cJSON *payload) {
    if (!initialized) {
        ESP_LOGE(TAG, "Mesh not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!payload) {
        ESP_LOGE(TAG, "Invalid argument: payload required");
        return ESP_ERR_INVALID_ARG;
    }

    esp_mesh_lite_msg_config_t config = {
        .json_msg = {.send_msg = "message", // Type must match msg_actions array
                     .expect_msg = NULL,    // No response expected
                     .max_retry = 0,
                     .retry_interval = 1000,
                     .req_payload = payload, // cJSON object, NOT string
                     .resend = is_mesh_root_node() ? &esp_mesh_lite_send_broadcast_msg_to_child
                                                   : &esp_mesh_lite_send_msg_to_root,
                     .send_fail = NULL}};

    char *json_str = cJSON_PrintUnformatted(payload);
    if (json_str) {
        ESP_LOGI(TAG, "Sending message: %s", json_str);
        free(json_str);
    }

    return esp_mesh_lite_send_msg(ESP_MESH_LITE_JSON_MSG, &config);
}
