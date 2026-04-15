#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#include "cJSON.h"
#include "esp_log.h"
#include "led_indicator.h"
#include "led_indicator_strips.h"
#include "led_strip_types.h"

#include "bits_helper.h"
#include "cmnd.h"
#include "led_indicator_adapter.h"
#include "supervisor.h"

#define TAG "cikon:adapter:led_indicator"

static bool initialized = false;
static led_indicator_handle_t led_handle = NULL;

static const blink_step_t pattern_fast_blinking[] = {
    {LED_BLINK_HSV, SET_IHSV(0, 30, 255, CONFIG_LED_INDICATOR_BRIGHTNESS), 0},
    {LED_BLINK_BRIGHTNESS, INSERT_INDEX(0, CONFIG_LED_INDICATOR_BRIGHTNESS), 100},
    {LED_BLINK_BRIGHTNESS, INSERT_INDEX(0, LED_STATE_OFF), 100},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t pattern_slow_pulse[] = {
    {LED_BLINK_HSV, SET_IHSV(0, 180, 255, CONFIG_LED_INDICATOR_BRIGHTNESS), 0}, // Cyan (H=180)
    {LED_BLINK_BRIGHTNESS, INSERT_INDEX(0, LED_STATE_OFF), 0},
    {LED_BLINK_BREATHE, INSERT_INDEX(0, CONFIG_LED_INDICATOR_BRIGHTNESS), 1000},
    {LED_BLINK_BREATHE, INSERT_INDEX(0, LED_STATE_OFF), 1000},
    {LED_BLINK_LOOP, 0, 0},
};

// Error - red double flash
// static const blink_step_t pattern_error[] = {
//     {LED_BLINK_RGB, SET_IRGB(0, 255, 0, 0), 0}, // Red
//     {LED_BLINK_BRIGHTNESS, INSERT_INDEX(0, LED_STATE_ON), 100},
//     {LED_BLINK_BRIGHTNESS, INSERT_INDEX(0, LED_STATE_OFF), 100},
//     {LED_BLINK_BRIGHTNESS, INSERT_INDEX(0, LED_STATE_ON), 100},
//     {LED_BLINK_BRIGHTNESS, INSERT_INDEX(0, LED_STATE_OFF), 700},
//     {LED_BLINK_LOOP, 0, 0},
// };

static const blink_step_t pattern_short_pulse[] = {
    {LED_BLINK_HSV, SET_IHSV(0, 120, 255, CONFIG_LED_INDICATOR_BRIGHTNESS), 0}, // Green (H=120)
    {LED_BLINK_BRIGHTNESS, INSERT_INDEX(0, CONFIG_LED_INDICATOR_BRIGHTNESS), 50},
    {LED_BLINK_BRIGHTNESS, INSERT_INDEX(0, LED_STATE_OFF), 2950},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t pattern_rainbow[] = {
    {LED_BLINK_HSV, SET_IHSV(0, 0, 255, CONFIG_LED_INDICATOR_BRIGHTNESS), 0},
    {LED_BLINK_HSV_RING, SET_IHSV(0, 360, 255, CONFIG_LED_INDICATOR_BRIGHTNESS), 3000},
    {LED_BLINK_LOOP, 0, 0},
};

// Special ACK pattern (preemptive) - blue double flash, auto-returns to previous effect
const blink_step_t pattern_ack[] = {
    {LED_BLINK_HSV, SET_IHSV(0, 240, 255, CONFIG_LED_INDICATOR_BRIGHTNESS), 0},
    {LED_BLINK_BRIGHTNESS, INSERT_INDEX(0, CONFIG_LED_INDICATOR_BRIGHTNESS), 60},
    {LED_BLINK_BRIGHTNESS, INSERT_INDEX(0, LED_STATE_OFF), 80},
    {LED_BLINK_BRIGHTNESS, INSERT_INDEX(0, CONFIG_LED_INDICATOR_BRIGHTNESS), 60},
    {LED_BLINK_HOLD, LED_STATE_OFF, 1000},
    {LED_BLINK_STOP, 0, 0},
};

// Blink type enumeration (priority order)
typedef enum {
    BLINK_INTERNET_READY = 0,
    BLINK_STA_READY,
    BLINK_PLATFORM_INITIALIZED,
    BLINK_BOOT,
    BLINK_ACK, // Preemptive acknowledgment flash
    BLINK_MAX
} led_indicator_blink_type_t;

// Blink list (order = priority, lower index = higher priority)
static blink_step_t const *led_blink_lists[] = {
    [BLINK_INTERNET_READY] = pattern_short_pulse,
    [BLINK_STA_READY] = pattern_slow_pulse,
    [BLINK_PLATFORM_INITIALIZED] = pattern_rainbow,
    [BLINK_BOOT] = pattern_fast_blinking,
    [BLINK_ACK] = pattern_ack,
    [BLINK_MAX] = NULL,
};

void led_set_status(led_indicator_handle_t led_handle, led_indicator_blink_type_t blink_type) {
    if (!led_handle) {
        ESP_LOGW(TAG, "LED handle is NULL, cannot set status");
        return;
    }
    if (blink_type >= BLINK_MAX) {
        return;
    }

    // Stop all effects before starting new one
    for (int i = 0; i < BLINK_MAX; i++) {
        led_indicator_stop(led_handle, i);
    }

    led_indicator_start(led_handle, blink_type);
}

static esp_err_t led_indicator_adapter_init(void) {
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing led_indicator adapter");

    // Configure SK6812 LED strip (GRB format, single LED on GPIO 35)
    led_indicator_strips_config_t strips_config = {
        .led_strip_cfg =
            {
                .strip_gpio_num = CONFIG_LED_INDICATOR_GPIO,
                .max_leds = 1,
                .led_model = LED_MODEL_SK6812,
                .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
                .flags.invert_out = false,
            },
        .led_strip_driver = LED_STRIP_RMT,
        .led_strip_rmt_cfg =
            {
                .clk_src = RMT_CLK_SRC_DEFAULT,
                .resolution_hz = 10 * 1000 * 1000, // 10 MHz
                .flags.with_dma = false,
            },
    };

    led_indicator_config_t config = {
        .blink_lists = led_blink_lists,
        .blink_list_num = BLINK_MAX,
    };

    esp_err_t ret = led_indicator_new_strips_device(&config, &strips_config, &led_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED indicator: %s", esp_err_to_name(ret));
        return ret;
    }

    initialized = true;
    ESP_LOGI(TAG, "LED indicator initialized on GPIO %d (SK6812)", CONFIG_LED_INDICATOR_GPIO);

    // Start with connecting status
    led_indicator_start(led_handle, BLINK_BOOT);
    // led_set_status(led_handle, BLINK_STARTUP);

    return ESP_OK;
}

static esp_err_t led_indicator_adapter_shutdown(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Shutting down led_indicator adapter");

    if (led_handle) {
        led_indicator_delete(&led_handle);
        led_handle = NULL;
    }

    initialized = false;
    return ESP_OK;
}

static void cmnd_led_indicator_effect(const char *args_json_str) {
    if (!initialized) {
        ESP_LOGW(TAG, "LED indicator not initialized");
        return;
    }

    cJSON *root = cJSON_Parse(args_json_str);
    if (!root || !cJSON_IsNumber(root)) {
        ESP_LOGW(TAG, "Invalid effect value: %s (expected number 0-%d)", args_json_str, BLINK_MAX);
        cJSON_Delete(root);
        return;
    }

    int effect_num = root->valueint;
    cJSON_Delete(root);

    if (effect_num < 0 || effect_num >= BLINK_MAX) {
        ESP_LOGW(TAG, "Effect out of range: %d (valid: 0-%d)", effect_num, BLINK_MAX);
        return;
    }

    ESP_LOGI(TAG, "Setting LED effect to: %d", effect_num);
    led_set_status(led_handle, (led_indicator_blink_type_t)effect_num);
}

static void led_indicator_adapter_on_event(EventBits_t bits) {
    if (!initialized) {
        return;
    }

    // if (bits & SUPERVISOR_EVENT_PLATFORM_INITIALIZED) {
    //     led_indicator_start(led_handle, BLINK_PLATFORM_INITIALIZED);
    // }

    if (bits & INET_EVENT_STA_READY) {
        led_indicator_start(led_handle, BLINK_STA_READY);
    }

    if (bits & INET_EVENT_STA_LOST) {
        led_indicator_stop(led_handle, BLINK_INTERNET_READY);
        led_indicator_stop(led_handle, BLINK_STA_READY);
    }

    if (bits & INET_INTERNET_READY) {
        led_indicator_start(led_handle, BLINK_INTERNET_READY);
    }

    if (bits & INET_INTERNET_LOST) {
        led_indicator_stop(led_handle, BLINK_INTERNET_READY);
    }

    if (bits & SUPERVISOR_EVENT_CMND_COMPLETED) {
        // Preemptive ACK flash - plays and auto-returns to previous effect
        led_indicator_preempt_start(led_handle, BLINK_ACK);
    }
}

static const command_entry_t led_indicator_commands[] = {
    {"effect", "Set LED effect (0=inet_ready, 1=sta_ready, 2=platform_init, 3=boot, 4=ack)",
     cmnd_led_indicator_effect},
    {NULL, NULL, NULL}};

supervisor_platform_adapter_t led_indicator_adapter = {
    .name = "led_indicator",
    .init = led_indicator_adapter_init,
    .shutdown = led_indicator_adapter_shutdown,
    .on_event = led_indicator_adapter_on_event,
    .on_interval = NULL,
    .tele_group = NULL,
    .cmnd_group = led_indicator_commands,
};
