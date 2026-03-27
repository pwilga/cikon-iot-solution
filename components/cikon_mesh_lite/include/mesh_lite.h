#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t mesh_id;
    const char *sta_ssid;
    const char *sta_password;
    const char *ap_ssid;
    const char *ap_password;
    const char *device_name;
} mesh_lite_config_t;

/**
 * @brief Callback for mesh message processing
 * @param payload Full JSON payload
 */
typedef void (*mesh_message_callback_t)(cJSON *payload);

void mesh_lite_configure(const mesh_lite_config_t *config);
esp_err_t mesh_lite_init(void);
esp_err_t mesh_lite_shutdown(void);
bool is_mesh_root_node(void);
void mesh_log_topology(void);

/**
 * @brief Register callback for processing mesh messages
 * @param callback Function to call when message is received
 */
void mesh_lite_register_message_callback(mesh_message_callback_t callback);

/**
 * @brief Send message through mesh network
 * @param payload JSON payload to send
 * @return ESP_OK on success
 */
esp_err_t mesh_lite_send_message(cJSON *payload);

#ifdef __cplusplus
}
#endif
