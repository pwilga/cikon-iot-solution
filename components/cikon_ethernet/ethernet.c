/*
 * SPDX-License-Identifier: MIT
 *
 * Cikon IoT Ethernet Hardware Abstraction Layer
 * Copyright (c) 2026 Piotr Wilga
 */

#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "ethernet.h"
#include "ethernet_backend.h"

#define TAG "cikon:ethernet"

static bool initialized = false;

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;

static const ethernet_backend_t *ethernet_get_backend(void) {
#ifdef CONFIG_CIKON_ETHERNET_W5500
    return &ethernet_backend_w5500;
#elif CONFIG_CIKON_ETHERNET_OPENETH
    return &ethernet_backend_openeth;
#else
#error                                                                                             \
    "No Ethernet backend configured! Select hardware in Kconfig (Component config -> Cikon Ethernet Hardware)."
#endif
}

esp_err_t ethernet_init(void) {
    if (initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const ethernet_backend_t *backend = ethernet_get_backend();
    ESP_LOGI(TAG, "Initializing Ethernet stack: %s", backend->name);

    // Step 0: Initialize TCP/IP adapter (required before creating netif)
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Step 1: Initialize hardware backend (SPI/MAC/PHY)
    ret = backend->init(&s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Backend init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Step 1.5: Set MAC address BEFORE creating netif (W5500 has no EEPROM)
    uint8_t mac[6];
    ret = esp_efuse_mac_get_default(mac);
    if (ret == ESP_OK) {
        ret = esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, mac);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set MAC address: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "MAC address set: " MACSTR, MAC2STR(mac));
        }
    } else {
        ESP_LOGW(TAG, "Failed to read base MAC: %s", esp_err_to_name(ret));
    }

    // Step 2: Create network interface
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&cfg);
    if (s_eth_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create esp_netif");
        backend->shutdown(s_eth_handle);
        s_eth_handle = NULL;
        return ESP_FAIL;
    }

    // Step 3: Create glue layer and attach netif to driver
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (s_eth_glue == NULL) {
        ESP_LOGE(TAG, "Failed to create netif glue");
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
        backend->shutdown(s_eth_handle);
        s_eth_handle = NULL;
        return ESP_FAIL;
    }

    ret = esp_netif_attach(s_eth_netif, s_eth_glue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach netif: %s", esp_err_to_name(ret));
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
        backend->shutdown(s_eth_handle);
        s_eth_handle = NULL;
        return ret;
    }

    // Step 5: Start driver (like all adapters - init does EVERYTHING)
    ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start driver: %s", esp_err_to_name(ret));
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
        backend->shutdown(s_eth_handle);
        s_eth_handle = NULL;
        return ret;
    }

    initialized = true;
    ESP_LOGI(TAG, "Ethernet stack initialized and started");
    return ESP_OK;
}

esp_err_t ethernet_shutdown(void) {
    if (!initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const ethernet_backend_t *backend = ethernet_get_backend();
    ESP_LOGI(TAG, "Shutting down Ethernet stack: %s", backend->name);

    // Step 1: Stop driver
    esp_eth_stop(s_eth_handle);

    // Step 2: Detach and destroy glue
    if (s_eth_glue) {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }

    // Step 3: Destroy network interface
    if (s_eth_netif) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }

    // Step 4: Shutdown hardware backend
    esp_err_t ret = backend->shutdown(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Backend shutdown failed: %s", esp_err_to_name(ret));
    }

    s_eth_handle = NULL;
    initialized = false;

    ESP_LOGI(TAG, "Ethernet shutdown complete");
    return ESP_OK;
}

const char *ethernet_get_backend_name(void) {
    const ethernet_backend_t *backend = ethernet_get_backend();
    return backend->name;
}

void ethernet_get_interface_ip(char *buf, size_t buflen) {
    if (!buf || buflen == 0)
        return;

    if (!s_eth_netif) {
        snprintf(buf, buflen, "0.0.0.0");
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_eth_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(buf, buflen, IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(buf, buflen, "0.0.0.0");
    }
}

esp_netif_t *ethernet_get_netif(void) { return s_eth_netif; }
