#include "config_manager.h"
#include "debug_adapter.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "mqtt.h"
#include "wifi.h"
#include <string.h>

#define TAG "cikon:adapter:debug"

static bool debug_enabled = true;

static void debug_print_config_summary(void) {
    const config_t *cfg = config_get();
    ESP_LOGI(TAG, "| * CONFIG *");
#define PRINT_STR(field, size, defval) ESP_LOGI(TAG, "| %-16s | %-36.36s |", #field, cfg->field);
#define PRINT_U8(field, defval) ESP_LOGI(TAG, "| %-16s | %-36u |", #field, (unsigned)cfg->field);
#define PRINT_U16(field, defval) ESP_LOGI(TAG, "| %-16s | %-36u |", #field, (unsigned)cfg->field);
    CONFIG_FIELDS(PRINT_STR, PRINT_U8, PRINT_U16)
#undef PRINT_STR
#undef PRINT_U8
#undef PRINT_U16
}

static void debug_print_tasks_summary(void) {
    TaskStatus_t *task_status_array;
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    task_status_array = malloc(task_count * sizeof(TaskStatus_t));
    ESP_LOGI(TAG, "| * TASKS *");
    if (task_status_array) {
        UBaseType_t real_count = uxTaskGetSystemState(task_status_array, task_count, NULL);
        char line[128] = "";
        int col = 0;
        for (UBaseType_t i = 0; i < real_count; i++) {
            char entry[40];
            snprintf(entry, sizeof(entry), "| %-14s %6u ", task_status_array[i].pcTaskName,
                     (unsigned)(task_status_array[i].usStackHighWaterMark * sizeof(StackType_t)));
            strcat(line, entry);
            col++;
            if (col == 3) {
                strcat(line, "|");
                ESP_LOGI(TAG, "%s", line);
                line[0] = '\0';
                col = 0;
            }
        }
        if (col > 0 && line[0]) {
            strcat(line, "|");
            ESP_LOGI(TAG, "%s", line);
        }
        free(task_status_array);
    }
}

static void debug_print_sys_info(void) {
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;

    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Get flash size failed");
        return;
    }

    ESP_LOGI(TAG, "This is %s chip with %d CPU core(s), %s%s%s%s, ", CONFIG_IDF_TARGET,
             chip_info.cores, (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
             (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
             (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");
    ESP_LOGI(TAG, "silicon revision v%d.%d, ", major_rev, minor_rev);
    ESP_LOGI(TAG, "%" PRIu32 "MB %s flash", flash_size / (uint32_t)(1024 * 1024),
             (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
}

// Adapter callbacks
static void debug_adapter_init(void) {
    ESP_LOGI(TAG, "Initializing debug adapter");
    debug_print_sys_info();
}

static void debug_adapter_on_event(EventBits_t bits) {

    if (!debug_enabled) {
        return;
    }
    // Log all events
    ESP_LOGI(TAG, "Event received: 0x%08" PRIx32, (uint32_t)bits);

    // Supervisor core events
    if (bits & SUPERVISOR_EVENT_CMND_COMPLETED) {
        ESP_LOGI(TAG, "  -> SUPERVISOR_EVENT_CMND_COMPLETED");
    }
    if (bits & SUPERVISOR_EVENT_RESERVED1) {
        ESP_LOGI(TAG, "  -> SUPERVISOR_EVENT_RESERVED1");
    }
    if (bits & SUPERVISOR_EVENT_RESERVED2) {
        ESP_LOGI(TAG, "  -> SUPERVISOR_EVENT_RESERVED2");
    }
    if (bits & SUPERVISOR_EVENT_RESERVED3) {
        ESP_LOGI(TAG, "  -> SUPERVISOR_EVENT_RESERVED3");
    }

    // Inet adapter events (BIT4-7)
    if (bits & BIT4) {
        ESP_LOGI(TAG, "  -> INET_EVENT_TIME_SYNCED");
    }
    if (bits & BIT5) {
        ESP_LOGI(TAG, "  -> INET_EVENT_STA_READY");
    }
    if (bits & BIT6) {
        ESP_LOGI(TAG, "  -> INET_EVENT_AP_READY");
    }
    if (bits & BIT7) {
        ESP_LOGI(TAG, "  -> INET_EVENT_RESERVED");
    }
}

static void debug_adapter_on_interval(supervisor_interval_stage_t stage) {
    if (!debug_enabled) {
        return;
    }

    // Log system stats every 2 seconds
    if (stage == SUPERVISOR_INTERVAL_2S) {
        size_t free_heap = esp_get_free_heap_size();
        ESP_LOGI(TAG, "Free heap: %.2f KB", free_heap / 1024.0);

        uint32_t uptime = esp_timer_get_time() / 1000000ULL;
        ESP_LOGI(TAG, "Uptime: %u s", uptime);

        wifi_log_event_group_bits();
        mqtt_log_event_group_bits();

        char ip[16];
        wifi_get_interface_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "IP: %s", ip);

        debug_print_tasks_summary();
        ESP_LOGI(TAG, "=====================");
    }
}

static void debug_adapter_shutdown(void) {
    ESP_LOGI(TAG, "Debug adapter shutdown - disabling periodic logging");
    debug_enabled = false;
}

supervisor_platform_adapter_t debug_adapter = {.init = debug_adapter_init,
                                               .shutdown = debug_adapter_shutdown,
                                               .on_event = debug_adapter_on_event,
                                               .on_interval = debug_adapter_on_interval};
