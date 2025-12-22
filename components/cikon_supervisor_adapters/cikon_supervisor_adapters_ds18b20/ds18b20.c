#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "esp_log.h"

#include "cJSON.h"

#include "ds18b20.h"
#include "ds18b20_adapter.h"
#include "json_parser.h"
#include "metadata.h"
#include "onewire_bus.h"
#include "supervisor.h"
#include "tele.h"

#define TAG "cikon:adapter:ds18b20"

typedef struct {
    ds18b20_device_handle_t handle;
    float last_temp;
    char name[32];
    uint64_t rom_id;
    bool valid;
} sensor_t;

static onewire_bus_handle_t bus = NULL;
static sensor_t sensors[CONFIG_DS18B20_MAX_SENSORS];
static uint8_t sensor_count = 0;
static bool initialized = false;

// Parse sensor names from Kconfig
static const char *sensor_names[CONFIG_DS18B20_MAX_SENSORS];
static uint8_t sensor_name_count = 0;

static void parse_sensor_names(void) {
    const char *name_list = CONFIG_DS18B20_SENSOR_NAMES;
    char *str = strdup(name_list);
    char *token = strtok(str, ",");
    sensor_name_count = 0;

    while (token != NULL && sensor_name_count < CONFIG_DS18B20_MAX_SENSORS) {
        sensor_names[sensor_name_count++] = strdup(token);
        token = strtok(NULL, ",");
    }
    free(str);
    ESP_LOGI(TAG, "Parsed %d sensor names from Kconfig", sensor_name_count);
}

// Comparator for qsort - sort sensors by ROM ID (deterministic ordering)
static int compare_sensors_by_rom(const void *a, const void *b) {
    const sensor_t *sa = (const sensor_t *)a;
    const sensor_t *sb = (const sensor_t *)b;
    if (sa->rom_id < sb->rom_id)
        return -1;
    if (sa->rom_id > sb->rom_id)
        return 1;
    return 0;
}

// Scan 1-Wire bus and register sensors
static void ds18b20_scan_sensors(void) {
    onewire_device_iter_handle_t iter = NULL;
    onewire_device_t next_onewire_device;
    ds18b20_config_t ds_cfg = {};
    esp_err_t search_result = ESP_OK;
    uint8_t found = 0;

    ESP_LOGI(TAG, "Scanning 1-Wire bus on GPIO %d", CONFIG_DS18B20_GPIO);

    ESP_ERROR_CHECK(onewire_new_device_iter(bus, &iter));
    // ESP_LOGI(TAG, "Device iterator created, start searching...");

    uint8_t max_iterations = CONFIG_DS18B20_MAX_SENSORS + 5; // Safety limit
    uint8_t iterations = 0;

    do {
        search_result = onewire_device_iter_get_next(iter, &next_onewire_device);
        if (search_result == ESP_OK) {
            ds18b20_device_handle_t ds18b20;
            if (ds18b20_new_device_from_enumeration(&next_onewire_device, &ds_cfg, &ds18b20) ==
                ESP_OK) {
                sensors[found].handle = ds18b20;
                sensors[found].valid = false;
                sensors[found].last_temp = 0.0f;
                sensors[found].rom_id = next_onewire_device.address; // Store ROM ID

                ESP_LOGI(TAG, "Found DS18B20[%d]: ROM=%016llX", found, next_onewire_device.address);
                found++;

                if (found >= CONFIG_DS18B20_MAX_SENSORS) {
                    ESP_LOGW(TAG, "Max sensor limit reached, stop searching");
                    break;
                }
            } else {
                ESP_LOGI(TAG, "Found unknown device, address: %016llX",
                         next_onewire_device.address);
            }
        }

        iterations++;
        if (iterations >= max_iterations) {
            ESP_LOGW(TAG, "Max search iterations reached, stopping");
            break;
        }
    } while (search_result != ESP_ERR_NOT_FOUND);

    ESP_ERROR_CHECK(onewire_del_device_iter(iter));
    sensor_count = found;

    // Sort sensors by ROM ID for deterministic ordering
    if (sensor_count > 1) {
        qsort(sensors, sensor_count, sizeof(sensor_t), compare_sensors_by_rom);
        ESP_LOGI(TAG, "Sensors sorted by ROM ID");
    }

    // Assign names from Kconfig or auto-generate
    for (uint8_t i = 0; i < sensor_count; i++) {
        if (i < sensor_name_count && sensor_names[i] != NULL) {
            strncpy(sensors[i].name, sensor_names[i], sizeof(sensors[i].name) - 1);
            sensors[i].name[sizeof(sensors[i].name) - 1] = '\0';
        } else {
            snprintf(sensors[i].name, sizeof(sensors[i].name), "temp%d", i);
        }
        ESP_LOGI(TAG, "Sensor[%d]: ROM=%016llX Name='%s'", i, sensors[i].rom_id, sensors[i].name);
    }
}

static void ds18b20_read_sensors(void) {

    if (!initialized || sensor_count == 0) {
        return;
    }

    ds18b20_trigger_temperature_conversion_for_all(bus);

    for (uint8_t i = 0; i < sensor_count; i++) {
        float temp;
        if (ds18b20_get_temperature(sensors[i].handle, &temp) == ESP_OK) {
            sensors[i].last_temp = temp;
            sensors[i].valid = true;
            ESP_LOGD(TAG, "Sensor '%s': %.2f°C", sensors[i].name, temp);
        } else {
            sensors[i].valid = false;
            ESP_LOGW(TAG, "Failed to read sensor '%s'", sensors[i].name);
        }
    }
}

static void tele_ds18b20_temps(const char *tele_id, cJSON *json_root) {
    cJSON *temp_obj = cJSON_CreateObject();

    for (uint8_t i = 0; i < sensor_count; i++) {
        if (sensors[i].valid) {
            char *sanitized_name = sanitize(sensors[i].name);
            cJSON_AddNumberToObject(temp_obj, sanitized_name, sensors[i].last_temp);
            free(sanitized_name);
        }
    }

    cJSON_AddItemToObject(json_root, tele_id, temp_obj);
}

static void ds18b20_adapter_init(void) {

    ESP_LOGI(TAG, "Initializing DS18B20 adapter");

    parse_sensor_names();

    // Install 1-Wire bus
    onewire_bus_config_t bus_config = {
        .bus_gpio_num = CONFIG_DS18B20_GPIO,
    };
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10, // 1byte ROM command + 8byte ROM number + 1byte device command
    };

    esp_err_t ret = onewire_new_bus_rmt(&bus_config, &rmt_config, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create 1-Wire bus: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "DS18B20 adapter initialization failed - check GPIO %d and RMT availability",
                 CONFIG_DS18B20_GPIO);
        return;
    }

    ds18b20_scan_sensors();
    ds18b20_read_sensors();

    initialized = true;

    ESP_LOGI(TAG, "DS18B20 adapter initialized with %d sensor(s)", sensor_count);
}

static void ds18b20_adapter_shutdown(void) {
    if (!initialized) {
        return;
    }

    ESP_LOGI(TAG, "Shutting down DS18B20 adapter");

    for (uint8_t i = 0; i < sensor_count; i++) {
        if (sensors[i].handle) {
            ds18b20_del_device(sensors[i].handle);
            sensors[i].handle = NULL;
        }
    }

    if (bus) {
        onewire_bus_del(bus);
        bus = NULL;
    }

    sensor_count = 0;
    initialized = false;

    ESP_LOGI(TAG, "DS18B20 adapter shut down");
}

static void ds18b20_adapter_on_interval(supervisor_interval_stage_t stage) {

    if (stage == SUPERVISOR_INTERVAL_10S) {
        ds18b20_read_sensors();
    }
}

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
#ifndef HA_ENTITY_LIST
#define HA_ENTITY_LIST // Fallback if CMake didn't inject
#endif

#define HA_ENTITY_ENTRY(sensor_name)                                                               \
    {.type = HA_SENSOR, .name = sensor_name, .device_class = "temperature", .parent_key = "temps"},

static const ha_metadata_t ds18b20_ha_metadata = {
    .magic = HA_METADATA_MAGIC, .entities = {HA_ENTITY_LIST{.type = HA_ENTITY_NONE}}};
#undef HA_ENTITY_ENTRY
#endif

supervisor_platform_adapter_t ds18b20_adapter = {
    .init = ds18b20_adapter_init,
    .shutdown = ds18b20_adapter_shutdown,
    .on_interval = ds18b20_adapter_on_interval,
    .tele_group = (const tele_entry_t[]){{"temps", tele_ds18b20_temps}, {NULL, NULL}},
    .cmnd_group = NULL,
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    .metadata = &ds18b20_ha_metadata,
#endif
};
