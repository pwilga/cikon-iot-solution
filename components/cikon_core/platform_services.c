#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "platform_services.h"

#define TAG "cikon-systems"

static void (*restart_callback)(void) = NULL;

static bool onboard_led_state = true;

void core_system_init(void) {

    ESP_ERROR_CHECK(nvs_flash_safe_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

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

const char *get_client_id(void) {

    static char buf[13] = {0};
    static bool initialized = false;

    if (initialized)
        return buf;

    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK)
        return NULL;

    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", MAC2STR(mac));

    initialized = true;
    return buf;
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
