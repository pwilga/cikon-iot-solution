#include "sdkconfig.h"

#ifdef CONFIG_CIKON_ETHERNET_W5500

#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_log.h"
#include "ethernet_backend.h"

#define TAG "cikon:ethernet:w5500"

static bool spi_bus_initialized = false;

static esp_err_t w5500_init_spi_bus(void) {
    if (spi_bus_initialized) {
        ESP_LOGD(TAG, "SPI bus already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SPI bus (host=%d, SCLK=%d, MOSI=%d, MISO=%d)",
             CONFIG_W5500_SPI_HOST, CONFIG_W5500_SPI_SCLK_GPIO, CONFIG_W5500_SPI_MOSI_GPIO,
             CONFIG_W5500_SPI_MISO_GPIO);

    // Install GPIO ISR service if using interrupt mode
#if CONFIG_W5500_INT_GPIO >= 0
    static bool gpio_isr_installed = false;
    if (!gpio_isr_installed) {
        esp_err_t ret = gpio_install_isr_service(0);
        if (ret == ESP_OK) {
            gpio_isr_installed = true;
            ESP_LOGD(TAG, "GPIO ISR service installed");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            // Already installed (e.g., by another component)
            ESP_LOGD(TAG, "GPIO ISR service already installed");
            gpio_isr_installed = true;
        } else {
            ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(ret));
            return ret;
        }
    }
#endif

    // Initialize SPI bus
    spi_bus_config_t buscfg = {
        .miso_io_num = CONFIG_W5500_SPI_MISO_GPIO,
        .mosi_io_num = CONFIG_W5500_SPI_MOSI_GPIO,
        .sclk_io_num = CONFIG_W5500_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    ESP_RETURN_ON_ERROR(spi_bus_initialize(CONFIG_W5500_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG,
                        "SPI host #%d init failed", CONFIG_W5500_SPI_HOST);

    spi_bus_initialized = true;
    ESP_LOGI(TAG, "SPI bus initialized successfully");
    return ESP_OK;
}

static esp_err_t w5500_deinit_spi_bus(void) {
    if (!spi_bus_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing SPI bus");
    esp_err_t ret = spi_bus_free(CONFIG_W5500_SPI_HOST);
    if (ret == ESP_OK) {
        spi_bus_initialized = false;
    }
    return ret;
}

static esp_err_t w5500_backend_init(esp_eth_handle_t *out_handle) {
    ESP_LOGI(TAG, "Initializing W5500 SPI Ethernet controller");
    ESP_LOGI(TAG, "Configuration: CS=%d, INT=%d, RST=%d, PHY_ADDR=%d", CONFIG_W5500_SPI_CS_GPIO,
             CONFIG_W5500_INT_GPIO, CONFIG_W5500_PHY_RST_GPIO, CONFIG_W5500_PHY_ADDR);

    // Step 1: Initialize SPI bus
    ESP_RETURN_ON_ERROR(w5500_init_spi_bus(), TAG, "SPI bus init failed");

    // Step 2: Configure SPI device for W5500
    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = CONFIG_W5500_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = CONFIG_W5500_SPI_CS_GPIO,
    };

    // Step 3: Create W5500-specific MAC configuration
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(CONFIG_W5500_SPI_HOST, &devcfg);

    // Configure interrupt or polling mode
#if CONFIG_W5500_INT_GPIO >= 0
    w5500_config.int_gpio_num = CONFIG_W5500_INT_GPIO;
    ESP_LOGI(TAG, "Using interrupt mode (GPIO %d)", CONFIG_W5500_INT_GPIO);
#else
    w5500_config.int_gpio_num = -1;
    w5500_config.poll_period_ms = CONFIG_W5500_POLLING_MS;
    ESP_LOGI(TAG, "Using polling mode (%d ms)", CONFIG_W5500_POLLING_MS);
#endif

    // Step 4: Create common MAC and PHY configs
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    phy_config.phy_addr = CONFIG_W5500_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_W5500_PHY_RST_GPIO;

    // Step 5: Create W5500 MAC and PHY instances
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "Failed to create W5500 MAC");

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "Failed to create W5500 PHY");
        mac->del(mac);
        return ESP_FAIL;
    }

    // Step 6: Install Ethernet driver
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
    ESP_LOGI(TAG, "W5500 initialized successfully");
    return ESP_OK;
}

static esp_err_t w5500_backend_shutdown(esp_eth_handle_t handle) {
    ESP_LOGI(TAG, "Shutting down W5500");

    // Step 1: Get MAC and PHY instances before uninstalling driver
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    esp_eth_get_mac_instance(handle, &mac);
    esp_eth_get_phy_instance(handle, &phy);

    // Step 2: Uninstall driver
    ESP_RETURN_ON_ERROR(esp_eth_driver_uninstall(handle), TAG, "Ethernet driver uninstall failed");

    // Step 3: Delete MAC and PHY instances
    if (mac != NULL) {
        mac->del(mac);
    }
    if (phy != NULL) {
        phy->del(phy);
    }

    // Step 4: Deinitialize SPI bus
    w5500_deinit_spi_bus();

    ESP_LOGI(TAG, "W5500 shutdown complete");
    return ESP_OK;
}

const ethernet_backend_t ethernet_backend_w5500 = {
    .init = w5500_backend_init, .shutdown = w5500_backend_shutdown, .name = "W5500"};

#endif // CONFIG_CIKON_ETHERNET_W5500
