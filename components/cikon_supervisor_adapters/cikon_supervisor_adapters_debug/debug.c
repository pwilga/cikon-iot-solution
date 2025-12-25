#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"

#include "config_manager.h"
#include "debug_adapter.h"
#include "enum_helpers.h"
#include "metadata.h"
#include "tele.h"

#define TAG "cikon:adapter:debug"

static bool debug_enabled = true;
static const esp_partition_t *failed_ota_partition = NULL;
static esp_ota_img_states_t failed_ota_state = ESP_OTA_IMG_UNDEFINED;
static char failed_ota_version[32] = "unknown";

// Weak symbols for optional WiFi diagnostics (zero coupling)
__attribute__((weak)) void wifi_log_event_group_bits(void) {
    // Default: no-op if wifi component not linked
}

__attribute__((weak)) void wifi_get_interface_ip(char *buf, size_t len) {
    // Default: "N/A" if wifi component not linked
    if (buf && len > 0) {
        strncpy(buf, "N/A", len - 1);
        buf[len - 1] = '\0';
    }
}

// Weak symbol for optional MQTT diagnostics (zero coupling)
__attribute__((weak)) void mqtt_log_event_group_bits(void) {
    // Default: no-op if mqtt component not linked
}

// Helper for random float generation
static float random_float(float min, float max) {
    return min + ((float)esp_random() / UINT32_MAX) * (max - min);
}

// Unified task data collection function
static TaskStatus_t *get_task_status_array(UBaseType_t *out_count) {
    UBaseType_t num_tasks = uxTaskGetNumberOfTasks();
    TaskStatus_t *task_array = malloc(num_tasks * sizeof(TaskStatus_t));
    if (!task_array) {
        return NULL;
    }
    *out_count = uxTaskGetSystemState(task_array, num_tasks, NULL);
    return task_array;
}

static void debug_print_config_summary(void) {
    const config_t *cfg = config_get();
    ESP_LOGI(TAG, "| * CONFIG *");
#define PRINT_STR(field, size, defval) ESP_LOGI(TAG, "| %-16s | %-36.36s |", #field, cfg->field);
#define PRINT_U8(field, defval) ESP_LOGI(TAG, "| %-16s | %-36u |", #field, (unsigned)cfg->field);
#define PRINT_U16(field, defval) ESP_LOGI(TAG, "| %-16s | %-36u |", #field, (unsigned)cfg->field);
#define PRINT_U32(field, defval) ESP_LOGI(TAG, "| %-16s | %-36u |", #field, (unsigned)cfg->field);
#define PRINT_U64(field, defval)                                                                   \
    ESP_LOGI(TAG, "| %-16s | %-36llu |", #field, (unsigned long long)cfg->field);
    CONFIG_FIELDS(PRINT_STR, PRINT_U8, PRINT_U16, PRINT_U32, PRINT_U64)
#undef PRINT_STR
#undef PRINT_U8
#undef PRINT_U16
#undef PRINT_U32
#undef PRINT_U64
}

static void debug_print_tasks_summary(void) {
    UBaseType_t task_count = 0;
    TaskStatus_t *task_status_array = get_task_status_array(&task_count);

    if (!task_status_array) {
        return;
    }

    ESP_LOGI(TAG, "| * TASKS *");
    char line[128] = "";
    int col = 0;
    for (UBaseType_t i = 0; i < task_count; i++) {
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

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
// Custom HA builder for tasks_dict
static void build_tasks_dict_ha(cJSON *payload, const char *sanitized_name) {
    char buf[64];

    snprintf(buf, sizeof(buf), "{{ value_json.%s | count }}", sanitized_name);
    cJSON_ReplaceItemInObject(payload, "val_tpl", cJSON_CreateString(buf));

    snprintf(buf, sizeof(buf), "{{ value_json.%s | tojson }}", sanitized_name);
    cJSON_AddStringToObject(payload, "json_attr_tpl", buf);
    cJSON_AddStringToObject(payload, "json_attr_t", "~/tele");
}
#endif

static void debug_adapter_init(void) {
    ESP_LOGI(TAG, "Initializing debug adapter");

    // Check for failed OTA partition (passive check only)
    failed_ota_partition = esp_ota_get_last_invalid_partition();

    if (failed_ota_partition != NULL) {
        esp_ota_get_state_partition(failed_ota_partition, &failed_ota_state);

        esp_app_desc_t app_desc;
        if (esp_ota_get_partition_description(failed_ota_partition, &app_desc) == ESP_OK) {
            snprintf(failed_ota_version, sizeof(failed_ota_version), "%s", app_desc.version);
        }
    }
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
    if (bits & SUPERVISOR_EVENT_PLATFORM_INITIALIZED) {
        ESP_LOGI(TAG, "  -> SUPERVISOR_EVENT_PLATFORM_INITIALIZED");
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
        if (supervisor_is_safe_mode_active()) {
            ESP_LOGE(TAG, "SAFE MODE ACTIVE - limited functionality");
        }

        size_t free_heap = esp_get_free_heap_size();
        ESP_LOGI(TAG, "Free heap: %.2f KB", free_heap / 1024.0);

        uint32_t uptime = esp_timer_get_time() / 1000000ULL;
        ESP_LOGI(TAG, "Uptime: %u s", uptime);

        wifi_log_event_group_bits();
        mqtt_log_event_group_bits();

        char ip[16];
        wifi_get_interface_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "IP: %s", ip);

        // Alert continuously if failed OTA detected (passive reporting)
        if (failed_ota_partition != NULL) {
            ESP_LOGW(TAG, "OTA rollback detected from %s partition: %s (v%s)",
                     failed_ota_partition->label, esp_ota_state_to_string(failed_ota_state),
                     failed_ota_version);
        }

        debug_print_tasks_summary();
        ESP_LOGI(TAG, "=====================");
    }
}

static void debug_adapter_shutdown(void) {
    ESP_LOGI(TAG, "Debug adapter shutdown - disabling periodic logging");
    debug_enabled = false;
}

static void tele_debug_temperature(const char *tele_id, cJSON *json_root) {
    float temp = random_float(20.5f, 25.9f);
    cJSON_AddNumberToObject(json_root, tele_id, temp);
}

static void tele_debug_rollback(const char *tele_id, cJSON *json_root) {
    if (!json_root)
        return;

    if (failed_ota_partition == NULL) {
        cJSON_AddStringToObject(json_root, tele_id, "n/a");
        return;
    }

    char rollback_str[64];
    snprintf(rollback_str, sizeof(rollback_str), "%s: %s", failed_ota_partition->label,
             esp_ota_state_to_string(failed_ota_state));
    cJSON_AddStringToObject(json_root, tele_id, rollback_str);
}

static void tele_debug_tasks_dict(const char *tele_id, cJSON *json_root) {
    if (!json_root)
        return;

    UBaseType_t task_count = 0;
    TaskStatus_t *task_status_array = get_task_status_array(&task_count);
    if (!task_status_array)
        return;

    cJSON *task_dict = cJSON_CreateObject();
    if (!task_dict) {
        free(task_status_array);
        return;
    }

    for (UBaseType_t i = 0; i < task_count; i++) {
        cJSON *json_task = cJSON_CreateObject();
        if (!json_task)
            continue;

        cJSON_AddNumberToObject(json_task, "prio", task_status_array[i].uxCurrentPriority);
        cJSON_AddNumberToObject(json_task, "stack", task_status_array[i].usStackHighWaterMark);
        cJSON_AddNumberToObject(json_task, "runtime_ticks", task_status_array[i].ulRunTimeCounter);
        cJSON_AddNumberToObject(json_task, "task_number", task_status_array[i].xTaskNumber);

        const char *state_str = "unknown";
        switch (task_status_array[i].eCurrentState) {
        case eRunning:
            state_str = "running";
            break;
        case eReady:
            state_str = "ready";
            break;
        case eBlocked:
            state_str = "blocked";
            break;
        case eSuspended:
            state_str = "suspended";
            break;
        case eDeleted:
            state_str = "deleted";
            break;
        default:
            break;
        }
        cJSON_AddStringToObject(json_task, "state", state_str);

#if (INCLUDE_xTaskGetAffinity == 1)
        cJSON_AddNumberToObject(json_task, "core", task_status_array[i].xCoreID);
#endif

        cJSON_AddItemToObject(task_dict, task_status_array[i].pcTaskName, json_task);
    }

    free(task_status_array);
    cJSON_AddItemToObject(json_root, tele_id, task_dict);
}

static void cmnd_debug_sysinfo(const char *args_json_str) {
    (void)args_json_str;
    debug_print_sys_info();
}

static void cmnd_debug_config(const char *args_json_str) {
    (void)args_json_str;
    debug_print_config_summary();
}

static void cmnd_debug_crash(const char *args_json_str) {
    (void)args_json_str;
    abort();
}

static const command_entry_t debug_commands[] = {
    {"sysinfo", "Print system information", cmnd_debug_sysinfo},
    {"showconf", "Print configuration summary", cmnd_debug_config},
    {"crash", "Crash the system (for testing)", cmnd_debug_crash},
    {NULL, NULL, NULL}};

static const tele_entry_t debug_telemetry[] = {{"temperature", tele_debug_temperature},
                                               {"tasks_dict", tele_debug_tasks_dict},
                                               {"rollback", tele_debug_rollback},
                                               {NULL, NULL}};

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
static const ha_metadata_t debug_ha_metadata = {
    .magic = HA_METADATA_MAGIC,
    .entities = {
        {.type = HA_SENSOR, .name = "Temperature", .device_class = "temperature"},
        {.type = HA_SENSOR,
         .name = "Tasks Dict",
         .entity_category = "diagnostic",
         .custom_builder = build_tasks_dict_ha},
        {.type = HA_ENTITY_NONE} // Sentinel
    }};
#endif

supervisor_platform_adapter_t debug_adapter = {
    .init = debug_adapter_init,
    .shutdown = debug_adapter_shutdown,
    .on_event = debug_adapter_on_event,
    .on_interval = debug_adapter_on_interval,
    .tele_group = debug_telemetry,
    .cmnd_group = debug_commands,
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    .metadata = &debug_ha_metadata,
#endif
};
