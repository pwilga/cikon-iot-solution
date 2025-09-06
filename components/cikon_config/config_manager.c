#include <string.h>
#include <sdkconfig.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "cJSON.h"

#include "config_manager.h"

#define TAG "cikon-config-manager"
#define CONFIG_MANAGER_NAMESPACE "config_mgr"

static config_t config_data;

#define NVS_OPEN_OR_RETURN()                                                                       \
    do {                                                                                           \
        esp_err_t err = nvs_open(CONFIG_MANAGER_NAMESPACE, NVS_READWRITE, &nvs);                   \
        if (err != ESP_OK) {                                                                       \
            ESP_LOGE(TAG, "NVS open error: %s", esp_err_to_name(err));                             \
            return err;                                                                            \
        }                                                                                          \
    } while (0)
#define NVS_COMMIT_AND_CLOSE()                                                                     \
    do {                                                                                           \
        esp_err_t commit_err = nvs_commit(nvs);                                                    \
        if (commit_err != ESP_OK) {                                                                \
            ESP_LOGE(TAG, "NVS commit error: %s", esp_err_to_name(commit_err));                    \
        }                                                                                          \
        nvs_close(nvs);                                                                            \
    } while (0)

#define GEN_SETTER_STR(field, size, defval)                                                        \
    esp_err_t config_set_##field(const char *val) {                                                \
        strncpy(config_data.field, val, size);                                                     \
        config_data.field[size - 1] = '\0';                                                        \
        nvs_handle_t nvs;                                                                          \
        NVS_OPEN_OR_RETURN();                                                                      \
        esp_err_t err = nvs_set_str(nvs, #field, config_data.field);                               \
        if (err != ESP_OK) {                                                                       \
            ESP_LOGE(TAG, "SAVE_STR: %s = '%s', nvs_set_str err: %s", #field, config_data.field,   \
                     esp_err_to_name(err));                                                        \
        }                                                                                          \
        NVS_COMMIT_AND_CLOSE();                                                                    \
        return ESP_OK;                                                                             \
    }
#define GEN_SETTER_U8(field, defval)                                                               \
    esp_err_t config_set_##field(uint8_t val) {                                                    \
        config_data.field = val;                                                                   \
        nvs_handle_t nvs;                                                                          \
        NVS_OPEN_OR_RETURN();                                                                      \
        esp_err_t err = nvs_set_u8(nvs, #field, config_data.field);                                \
        if (err != ESP_OK) {                                                                       \
            ESP_LOGE(TAG, "SAVE_U8: %s = %u, nvs_set_u8 err: %s", #field,                          \
                     (unsigned)config_data.field, esp_err_to_name(err));                           \
        }                                                                                          \
        NVS_COMMIT_AND_CLOSE();                                                                    \
        return ESP_OK;                                                                             \
    }
#define GEN_SETTER_U16(field, defval)                                                              \
    esp_err_t config_set_##field(uint16_t val) {                                                   \
        config_data.field = val;                                                                   \
        nvs_handle_t nvs;                                                                          \
        NVS_OPEN_OR_RETURN();                                                                      \
        esp_err_t err = nvs_set_u16(nvs, #field, config_data.field);                               \
        if (err != ESP_OK) {                                                                       \
            ESP_LOGE(TAG, "SAVE_U16: %s = %u, nvs_set_u16 err: %s", #field,                        \
                     (unsigned)config_data.field, esp_err_to_name(err));                           \
        }                                                                                          \
        NVS_COMMIT_AND_CLOSE();                                                                    \
        return ESP_OK;                                                                             \
    }
CONFIG_FIELDS(GEN_SETTER_STR, GEN_SETTER_U8, GEN_SETTER_U16)
#undef GEN_SETTER_STR
#undef GEN_SETTER_U8
#undef GEN_SETTER_U16

void config_manager_init(void) {
    nvs_handle_t nvs;
    esp_err_t nvs_ok = nvs_open(CONFIG_MANAGER_NAMESPACE, NVS_READONLY, &nvs);
    if (nvs_ok != ESP_OK) {
        ESP_LOGW(TAG, "NVS open error: %s (loading defaults)", esp_err_to_name(nvs_ok));

#define STR(field, size, defval)                                                                   \
    strncpy(config_data.field, defval, size);                                                      \
    config_data.field[size - 1] = '\0';
#define U8(field, defval) config_data.field = defval;
#define U16(field, defval) config_data.field = defval;
        CONFIG_FIELDS(STR, U8, U16);
#undef STR
#undef U8
#undef U16
        return;
    }
    size_t len;

#define STR(field, size, defval)                                                                   \
    len = size;                                                                                    \
    {                                                                                              \
        esp_err_t err = nvs_get_str(nvs, #field, config_data.field, &len);                         \
        if (err != ESP_OK) {                                                                       \
            if (err != ESP_ERR_NVS_NOT_FOUND) {                                                    \
                ESP_LOGE(TAG, "LOAD_STR: %s (default), nvs_get_str err: %s", #field,               \
                         esp_err_to_name(err));                                                    \
            }                                                                                      \
            strncpy(config_data.field, defval, size);                                              \
            config_data.field[size - 1] = '\0';                                                    \
        }                                                                                          \
    }
#define U8(field, defval)                                                                          \
    {                                                                                              \
        esp_err_t err = nvs_get_u8(nvs, #field, &config_data.field);                               \
        if (err != ESP_OK) {                                                                       \
            if (err != ESP_ERR_NVS_NOT_FOUND) {                                                    \
                ESP_LOGE(TAG, "LOAD_U8: %s = %u (default), nvs_get_u8 err: %s", #field,            \
                         (unsigned)(defval), esp_err_to_name(err));                                \
            }                                                                                      \
            config_data.field = defval;                                                            \
        }                                                                                          \
    }
#define U16(field, defval)                                                                         \
    {                                                                                              \
        esp_err_t err = nvs_get_u16(nvs, #field, &config_data.field);                              \
        if (err != ESP_OK) {                                                                       \
            if (err != ESP_ERR_NVS_NOT_FOUND) {                                                    \
                ESP_LOGE(TAG, "LOAD_U16: %s = %u (default), nvs_get_u16 err: %s", #field,          \
                         (unsigned)(defval), esp_err_to_name(err));                                \
            }                                                                                      \
            config_data.field = defval;                                                            \
        }                                                                                          \
    }
    CONFIG_FIELDS(STR, U8, U16);
#undef STR
#undef U8
#undef U16
    nvs_close(nvs);
}

const config_t *config_get(void) { return &config_data; }

void config_manager_print_all_keys(void) {

    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", CONFIG_MANAGER_NAMESPACE, NVS_TYPE_ANY, &it);

    ESP_LOGI(TAG, "NVS keys in namespace '%s':", CONFIG_MANAGER_NAMESPACE);
    if (err != ESP_OK || it == NULL) {
        ESP_LOGI(TAG, "No keys found or error: %s", esp_err_to_name(err));
        return;
    }
    while (err == ESP_OK && it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        ESP_LOGI(TAG, "key: %-15s | type: %d", info.key, info.type);
        err = nvs_entry_next(&it);
    }
}

#define CONFIG_FIELD_APPLY_SETTER_STR(field, size, default_val)                                    \
    if (strcmp(item->string, #field) == 0) {                                                       \
        if (item->type == cJSON_String) {                                                          \
            config_set_##field(item->valuestring);                                                 \
        } else {                                                                                   \
            ESP_LOGW(TAG, "JSON type mismatch for field %s (expected string)", #field);            \
        }                                                                                          \
        return 1;                                                                                  \
    }
#define CONFIG_FIELD_APPLY_SETTER_U8(field, default_val)                                           \
    if (strcmp(item->string, #field) == 0) {                                                       \
        if (item->type == cJSON_Number) {                                                          \
            config_set_##field(item->valueint);                                                    \
        } else {                                                                                   \
            ESP_LOGW(TAG, "JSON type mismatch for field %s (expected number)", #field);            \
        }                                                                                          \
        return 1;                                                                                  \
    }
#define CONFIG_FIELD_APPLY_SETTER_U16(field, default_val)                                          \
    if (strcmp(item->string, #field) == 0) {                                                       \
        if (item->type == cJSON_Number) {                                                          \
            config_set_##field(item->valueint);                                                    \
        } else {                                                                                   \
            ESP_LOGW(TAG, "JSON type mismatch for field %s (expected number)", #field);            \
        }                                                                                          \
        return 1;                                                                                  \
    }

static int config_manager_apply_json_item(const cJSON *item) {
    CONFIG_FIELDS(CONFIG_FIELD_APPLY_SETTER_STR, CONFIG_FIELD_APPLY_SETTER_U8,
                  CONFIG_FIELD_APPLY_SETTER_U16)
    return 0;
}

void config_manager_set_from_json(const cJSON *json) {
    for (const cJSON *item = json->child; item != NULL; item = item->next) {
        if (!item->string)
            continue;
        if (!config_manager_apply_json_item(item)) {
            ESP_LOGW(TAG, "Unknown configuration key: %s", item->string);
        }
    }
}
