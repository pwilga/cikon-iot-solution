#include <time.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "platform_services.h"

#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif
#if CONFIG_SPIRAM
#include "esp_heap_caps.h"
#endif
#if CONFIG_VFS_EVENTFD_MAX_FDS > 0
#include "esp_vfs_eventfd.h"
#endif
#if CONFIG_VFS_LITTLEFS_ENABLED
#include "esp_littlefs.h"
#endif
#if CONFIG_VFS_SPIFFS_ENABLED
#include "esp_spiffs.h"
#endif

#define TAG "cikon:platform"

static void (*restart_callback)(void) = NULL;

static bool onboard_led_state = true;

void core_system_init(void) {

    ESP_ERROR_CHECK(nvs_flash_safe_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_VFS_EVENTFD_MAX_FDS > 0
    esp_vfs_eventfd_config_t efd_cfg = {.max_fds = CONFIG_VFS_EVENTFD_MAX_FDS};
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&efd_cfg));
#endif

#if CONFIG_VFS_LITTLEFS_ENABLED
    {
        esp_vfs_littlefs_conf_t lfs = {
            .base_path = CONFIG_VFS_LITTLEFS_MOUNT_POINT,
            .partition_label = CONFIG_VFS_LITTLEFS_PARTITION,
            .format_if_mount_failed = false,
        };
        ESP_ERROR_CHECK(esp_vfs_littlefs_register(&lfs));
        ESP_LOGI(TAG, "LittleFS mounted at %s", CONFIG_VFS_LITTLEFS_MOUNT_POINT);
    }
#endif

#if CONFIG_VFS_SPIFFS_ENABLED
    {
        esp_vfs_spiffs_conf_t spiffs = {
            .base_path = CONFIG_VFS_SPIFFS_MOUNT_POINT,
            .partition_label = CONFIG_VFS_SPIFFS_PARTITION,
            .max_files = 10,
            .format_if_mount_failed = false,
        };
        ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs));
        ESP_LOGI(TAG, "SPIFFS mounted at %s", CONFIG_VFS_SPIFFS_MOUNT_POINT);
    }
#endif

    // onboard_led
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

static const char *chip_name(esp_chip_model_t m) {
    switch (m) {
    case CHIP_ESP32:
        return "esp32";
    case CHIP_ESP32S2:
        return "esp32s2";
    case CHIP_ESP32S3:
        return "esp32s3";
    case CHIP_ESP32C3:
        return "esp32c3";
    case CHIP_ESP32C6:
        return "esp32c6";
    case CHIP_ESP32H2:
        return "esp32h2";
    default:
        return "esp32";
    }
}

const device_info_t *get_device_info(void) {
    static device_info_t device_info_s = {0};
    static bool initialized = false;

    if (initialized)
        return &device_info_s;

    const esp_app_desc_t *desc = esp_app_get_description();
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);

    device_info_s.app_name = desc->project_name;
    device_info_s.app_version = desc->version;
    device_info_s.idf_version = desc->idf_ver;
    device_info_s.chip = chip_name(chip.model);
    device_info_s.chip_rev = chip.revision;
    device_info_s.cores = chip.cores;

    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(device_info_s.id, sizeof(device_info_s.id), "%02X%02X%02X%02X%02X%02X",
                 MAC2STR(mac));
    }

    initialized = true;
    return &device_info_s;
}

const char *get_boot_time(void) {

    static char iso8601[32] = {0};
    static bool initialized = false;

    if (initialized) {
        return iso8601;
    }

    time_t now_sec = 0;
    struct tm tm_now = {0};
    time(&now_sec);
    gmtime_r(&now_sec, &tm_now);
    int calendar_year = tm_now.tm_year + 1900;

    int64_t uptime_us = esp_timer_get_time();
    time_t boot_time = now_sec - (uptime_us / 1000000);

    strftime(iso8601, sizeof(iso8601), "%Y-%m-%dT%H:%M:%SZ", gmtime(&boot_time));

    // treat time as unsynced until year >= 2020 (likely after SNTP)
    if (calendar_year > 2020) {
        initialized = true;
    }

    return iso8601;
}

bool get_onboard_led_state(void) { return onboard_led_state; }

void onboard_led_set_state(bool state) {

    if (onboard_led_state == state) {
        return;
    }

    ESP_ERROR_CHECK(gpio_set_level(CONFIG_BOARD_STATUS_LED_GPIO, !state));
    onboard_led_state = state;
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

const char **get_chip_features(void) {
    static const char *feats[5] = {0};
    static bool initialized = false;
    if (initialized)
        return feats;
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    int i = 0;
    if (ci.features & CHIP_FEATURE_WIFI_BGN)
        feats[i++] = "wifi";
    if (ci.features & CHIP_FEATURE_BT)
        feats[i++] = "bt";
    if (ci.features & CHIP_FEATURE_BLE)
        feats[i++] = "ble";
    if (ci.features & CHIP_FEATURE_IEEE802154)
        feats[i++] = "ieee802154";
    feats[i] = NULL;
    initialized = true;
    return feats;
}

uint32_t get_flash_size(void) {
    uint32_t size = 0;
    esp_flash_get_size(NULL, &size);
    return size;
}

size_t get_psram_size(void) {
#if CONFIG_SPIRAM
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
#else
    return 0;
#endif
}

int get_cpu_freq_mhz(void) { return CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ; }

bool get_fs_info(size_t *used, size_t *total) {
#if CONFIG_VFS_LITTLEFS_ENABLED
    return esp_littlefs_info(CONFIG_VFS_LITTLEFS_PARTITION, total, used) == ESP_OK;
#else
    (void)used;
    (void)total;
    return false;
#endif
}

#if SOC_TEMP_SENSOR_SUPPORTED
static temperature_sensor_handle_t s_temp_sensor = NULL;

static void ensure_temp_sensor(void) {
    if (s_temp_sensor)
        return;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    if (temperature_sensor_install(&cfg, &s_temp_sensor) == ESP_OK)
        temperature_sensor_enable(s_temp_sensor);
}
#endif

bool get_chip_temp(float *out) {
#if SOC_TEMP_SENSOR_SUPPORTED
    ensure_temp_sensor();
    return s_temp_sensor && temperature_sensor_get_celsius(s_temp_sensor, out) == ESP_OK;
#else
    (void)out;
    return false;
#endif
}
