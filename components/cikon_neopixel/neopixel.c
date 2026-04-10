#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"

#include "esp_log.h"
#include "led_strip.h"

#include "neopixel.h"
#include "neopixel_colors.h"

#define TAG "cikon:neopixel"

static led_strip_handle_t s_strip = NULL;
static uint16_t s_count = 0;
static uint8_t s_brightness = 255;
static SemaphoreHandle_t s_rmt_sem = NULL;

esp_err_t neopixel_init(int gpio, uint16_t count) {
    if (s_strip) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing neopixel on GPIO %d, count=%d", gpio, count);

    led_strip_config_t strip_config = {.strip_gpio_num = gpio,
                                       .max_leds = count,
                                       .led_model = LED_MODEL_WS2812,
                                       .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
                                       .flags = {
                                           .invert_out = false,
                                       }};

    led_strip_rmt_config_t rmt_config = {.clk_src = RMT_CLK_SRC_DEFAULT,
                                         .resolution_hz = 10 * 1000 * 1000,
                                         .mem_block_symbols = 64,
                                         .flags = {
                                             .with_dma = false,
                                         }};

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create led_strip: %s", esp_err_to_name(err));
        return err;
    }

    s_count = count;

    // Binary semaphore - no priority inheritance, safe with vTaskDelete()
    s_rmt_sem = xSemaphoreCreateBinary();
    if (!s_rmt_sem) {
        led_strip_del(s_strip);
        s_strip = NULL;
        ESP_LOGE(TAG, "Failed to create semaphore");
        return ESP_FAIL;
    }
    xSemaphoreGive(s_rmt_sem); // Initial state: available

    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);

    ESP_LOGI(TAG, "Neopixel initialized successfully");
    return ESP_OK;
}

esp_err_t neopixel_deinit(void) {
    if (!s_strip) {
        return ESP_ERR_INVALID_STATE;
    }

    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
    led_strip_del(s_strip);
    s_strip = NULL;
    s_count = 0;

    if (s_rmt_sem) {
        vSemaphoreDelete(s_rmt_sem);
        s_rmt_sem = NULL;
    }

    return ESP_OK;
}

esp_err_t neopixel_set_pixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    if (!s_strip || idx >= s_count) {
        return ESP_ERR_INVALID_STATE;
    }

    // Apply brightness scaling
    r = (uint8_t)((r * s_brightness) / 255);
    g = (uint8_t)((g * s_brightness) / 255);
    b = (uint8_t)((b * s_brightness) / 255);

    return led_strip_set_pixel(s_strip, idx, r, g, b);
}

esp_err_t neopixel_fill(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_strip) {
        return ESP_ERR_INVALID_STATE;
    }

    for (uint16_t i = 0; i < s_count; i++) {
        esp_err_t err = neopixel_set_pixel(i, r, g, b);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t neopixel_clear(void) {
    if (!s_strip) {
        return ESP_ERR_INVALID_STATE;
    }

    return led_strip_clear(s_strip);
}

esp_err_t neopixel_show(void) {
    if (!s_strip || !s_rmt_sem) {
        return ESP_ERR_INVALID_STATE;
    }

    // Binary semaphore: safe timeout if vTaskDelete() leaves it locked
    if (xSemaphoreTake(s_rmt_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT; // RMT busy, skip frame
    }

    esp_err_t ret = led_strip_refresh(s_strip);
    xSemaphoreGive(s_rmt_sem);
    return ret;
}

uint16_t neopixel_get_count(void) { return s_count; }

void neopixel_set_brightness(uint8_t brightness) { s_brightness = brightness; }

uint8_t neopixel_get_brightness(void) { return s_brightness; }

uint32_t neopixel_hsv(uint16_t hue, uint8_t sat, uint8_t val) {
    // Normalize inputs
    hue = hue % 360;
    if (sat > 100)
        sat = 100;
    if (val > 100)
        val = 100;

    uint8_t region = hue / 60;
    uint8_t remainder = (hue % 60) * 255 / 60;

    uint8_t p = (val * (100 - sat)) / 100;
    uint8_t q = (val * (100 - (sat * remainder) / 255)) / 100;
    uint8_t t = (val * (100 - (sat * (255 - remainder)) / 255)) / 100;

    // Scale to 0-255
    val = (val * 255) / 100;
    p = (p * 255) / 100;
    q = (q * 255) / 100;
    t = (t * 255) / 100;

    uint8_t r, g, b;
    switch (region) {
    case 0:
        r = val;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = val;
        b = p;
        break;
    case 2:
        r = p;
        g = val;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = val;
        break;
    case 4:
        r = t;
        g = p;
        b = val;
        break;
    default: // case 5
        r = val;
        g = p;
        b = q;
        break;
    }

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
