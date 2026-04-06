#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#include "esp_log.h"
#include "led_strip.h"

#include "neopixel.h"

#define TAG "cikon:neopixel"

static led_strip_handle_t s_strip = NULL;
static uint16_t s_count = 0;
static uint8_t s_width = 1;
static uint8_t s_brightness = 255;

esp_err_t neopixel_init(int gpio, uint16_t count, uint8_t width) {
    if (s_strip) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing neopixel on GPIO %d, count=%d, width=%d", gpio, count, width);

    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio,
        .max_leds = count,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create led_strip: %s", esp_err_to_name(err));
        return err;
    }

    s_count = count;
    s_width = (width > 0) ? width : 1;

    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);

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

esp_err_t neopixel_set_xy(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b) {
    if (!s_strip) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t idx;

#if CONFIG_NEOPIXEL_MATRIX_SNAKE
    // Snake/zigzag mapping: odd rows are reversed
    if (y % 2 == 0) {
        idx = y * s_width + x;
    } else {
        idx = y * s_width + (s_width - 1 - x);
    }
#else
    idx = y * s_width + x;
#endif

    return neopixel_set_pixel(idx, r, g, b);
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
    if (!s_strip) {
        return ESP_ERR_INVALID_STATE;
    }

    return led_strip_refresh(s_strip);
}

uint16_t neopixel_get_count(void) { return s_count; }

uint8_t neopixel_get_width(void) { return s_width; }

void neopixel_set_brightness(uint8_t brightness) { s_brightness = brightness; }

uint8_t neopixel_get_brightness(void) { return s_brightness; }
