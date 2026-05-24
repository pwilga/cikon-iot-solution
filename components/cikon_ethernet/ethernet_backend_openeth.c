/*
 * SPDX-License-Identifier: MIT
 *
 * Cikon IoT Ethernet - ESP32-P4 OpenETH Backend
 * Copyright (c) 2026 Piotr Wilga
 */

#ifdef CONFIG_CIKON_ETHERNET_OPENETH

#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#include "esp_eth_phy.h"
#include "esp_log.h"
#include "ethernet_backend.h"

#define TAG "cikon:ethernet:openeth"

// ===== PHY Selection =====

static const char* openeth_get_phy_name(void) {
#ifdef CONFIG_CIKON_ETHERNET_PHY_IP101
    return "IP101";
#elif defined(CONFIG_CIKON_ETHERNET_PHY_GENERIC)
    return "Generic";
#else
    return "Unknown";
#endif
}

static esp_eth_phy_t* openeth_create_phy(const eth_phy_config_t *config) {
#ifdef CONFIG_CIKON_ETHERNET_PHY_IP101
    ESP_LOGI(TAG, "Creating IP101 PHY instance");
    return esp_eth_phy_new_ip101(config);
#elif defined(CONFIG_CIKON_ETHERNET_PHY_GENERIC)
    ESP_LOGI(TAG, "Creating Generic PHY instance (auto-detect)");
    return esp_eth_phy_new_generic(config);
#else
    #error "No PHY type selected! Configure in Kconfig (CIKON_ETHERNET_PHY_*)."
    return NULL;
#endif
}

// ===== OpenETH Backend Implementation =====

static esp_err_t openeth_backend_init(esp_eth_handle_t *out_handle) {
    ESP_LOGI(TAG, "Initializing ESP32-P4 OpenETH + %s PHY", openeth_get_phy_name());
    ESP_LOGI(TAG, "Configuration: PHY_ADDR=%d, RST_GPIO=%d",
             CONFIG_ETHERNET_PHY_ADDR,
             CONFIG_ETHERNET_PHY_RST_GPIO);
    
    // Step 1: Create OpenETH MAC configuration (ESP32-P4 internal MAC)
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_openeth_config_t openeth_config = ETH_ESP32_OPENETH_DEFAULT_CONFIG();
    
    // Step 2: Create OpenETH MAC instance
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&openeth_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "Failed to create OpenETH MAC");
    
    // Step 3: Create PHY configuration
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_ETHERNET_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_ETHERNET_PHY_RST_GPIO;
    
    // Step 4: Create PHY instance (IP101 or Generic)
    esp_eth_phy_t *phy = openeth_create_phy(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "Failed to create %s PHY", openeth_get_phy_name());
        mac->del(mac);
        return ESP_FAIL;
    }
    
    // Step 5: Install Ethernet driver
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    
    esp_err_t ret = esp_eth_driver_install(&config, &eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(ret));
        phy->del(phy);
        mac->del(mac);
        return ret;
    }
    
    *out_handle = eth_handle;
    ESP_LOGI(TAG, "OpenETH + %s PHY initialized successfully", openeth_get_phy_name());
    return ESP_OK;
}

static esp_err_t openeth_backend_shutdown(esp_eth_handle_t handle) {
    ESP_LOGI(TAG, "Shutting down OpenETH + %s PHY", openeth_get_phy_name());
    
    // Step 1: Get MAC and PHY instances before uninstalling driver
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    esp_eth_get_mac_instance(handle, &mac);
    esp_eth_get_phy_instance(handle, &phy);
    
    // Step 2: Uninstall driver
    ESP_RETURN_ON_ERROR(
        esp_eth_driver_uninstall(handle),
        TAG, "Ethernet driver uninstall failed"
    );
    
    // Step 3: Delete MAC and PHY instances
    if (mac != NULL) {
        mac->del(mac);
    }
    if (phy != NULL) {
        phy->del(phy);
    }
    
    ESP_LOGI(TAG, "OpenETH shutdown complete");
    return ESP_OK;
}

// ===== Backend Registration =====

const ethernet_backend_t ethernet_backend_openeth = {
    .init = openeth_backend_init,
    .shutdown = openeth_backend_shutdown,
    .name = "OpenETH"
};

#endif // CONFIG_CIKON_ETHERNET_OPENETH
