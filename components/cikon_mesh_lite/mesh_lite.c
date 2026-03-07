#include "mesh_lite.h"
#include "esp_bridge.h"
#include "esp_log.h"
#include "esp_mesh_lite.h"

#define TAG "cikon:mesh_lite"

esp_err_t mesh_lite_init(void) {
    ESP_LOGI(TAG, "Initializing ESP-MESH Lite");

    // TODO: Check examples in managed_components/espressif__esp-mesh-lite/examples/
    // TODO: esp_bridge_create_all_netif()
    // TODO: esp_mesh_lite_init()

    return ESP_OK;
}

esp_err_t mesh_lite_shutdown(void) {
    ESP_LOGI(TAG, "Shutting down ESP-MESH Lite");

    // TODO: cleanup

    return ESP_OK;
}
