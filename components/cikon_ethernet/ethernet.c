/*
 * SPDX-License-Identifier: MIT
 *
 * Cikon IoT Ethernet Hardware Abstraction Layer
 * Copyright (c) 2026 Piotr Wilga
 */

#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#include "esp_log.h"
#include "ethernet.h"
#include "ethernet_backend.h"

#define TAG "cikon:ethernet"

static bool initialized = false;
static esp_eth_handle_t s_eth_handle = NULL;

// ===== Backend Selection (Compile-Time) =====

static const ethernet_backend_t *ethernet_get_backend(void) {
#ifdef CONFIG_CIKON_ETHERNET_W5500
    return &ethernet_backend_w5500;
#elif CONFIG_CIKON_ETHERNET_OPENETH
    return &ethernet_backend_openeth;
#else
    #error "No Ethernet backend configured! Select hardware in Kconfig (Component config -> Cikon Ethernet Hardware)."
#endif
}

// ===== Public API =====

esp_err_t ethernet_init(esp_eth_handle_t *out_handle) {
    if (initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (out_handle == NULL) {
        ESP_LOGE(TAG, "out_handle cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    const ethernet_backend_t *backend = ethernet_get_backend();
    ESP_LOGI(TAG, "Initializing Ethernet backend: %s", backend->name);
    
    esp_err_t ret = backend->init(&s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Backend init failed: %s (%d)", esp_err_to_name(ret), ret);
        return ret;
    }
    
    initialized = true;
    *out_handle = s_eth_handle;
    
    ESP_LOGI(TAG, "Ethernet initialized successfully");
    return ESP_OK;
}

esp_err_t ethernet_shutdown(esp_eth_handle_t handle) {
    if (!initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (handle == NULL) {
        ESP_LOGE(TAG, "handle cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    const ethernet_backend_t *backend = ethernet_get_backend();
    ESP_LOGI(TAG, "Shutting down Ethernet backend: %s", backend->name);
    
    esp_err_t ret = backend->shutdown(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Backend shutdown failed: %s (%d)", esp_err_to_name(ret), ret);
        // Continue anyway to reset state
    }
    
    s_eth_handle = NULL;
    initialized = false;
    
    ESP_LOGI(TAG, "Ethernet shutdown complete");
    return ret;
}

const char *ethernet_get_backend_name(void) {
    const ethernet_backend_t *backend = ethernet_get_backend();
    return backend->name;
}
