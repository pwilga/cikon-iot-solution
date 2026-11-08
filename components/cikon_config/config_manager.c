#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"

#include "config_manager.h"

#define TAG "cikon:config"
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
#define GEN_SETTER_U32(field, defval)                                                              \
    esp_err_t config_set_##field(uint32_t val) {                                                   \
        config_data.field = val;                                                                   \
        nvs_handle_t nvs;                                                                          \
        NVS_OPEN_OR_RETURN();                                                                      \
        esp_err_t err = nvs_set_u32(nvs, #field, config_data.field);                               \
        if (err != ESP_OK) {                                                                       \
            ESP_LOGE(TAG, "SAVE_U32: %s = %u, nvs_set_u32 err: %s", #field,                        \
                     (unsigned)config_data.field, esp_err_to_name(err));                           \
        }                                                                                          \
        NVS_COMMIT_AND_CLOSE();                                                                    \
        return ESP_OK;                                                                             \
    }
#define GEN_SETTER_U64(field, defval)                                                              \
    esp_err_t config_set_##field(uint64_t val) {                                                   \
        config_data.field = val;                                                                   \
        nvs_handle_t nvs;                                                                          \
        NVS_OPEN_OR_RETURN();                                                                      \
        esp_err_t err = nvs_set_u64(nvs, #field, config_data.field);                               \
        if (err != ESP_OK) {                                                                       \
            ESP_LOGE(TAG, "SAVE_U64: %s = %llu, nvs_set_u64 err: %s", #field,                      \
                     (unsigned long long)config_data.field, esp_err_to_name(err));                 \
        }                                                                                          \
        NVS_COMMIT_AND_CLOSE();                                                                    \
        return ESP_OK;                                                                             \
    }
CONFIG_FIELDS(GEN_SETTER_STR, GEN_SETTER_U8, GEN_SETTER_U16, GEN_SETTER_U32, GEN_SETTER_U64)
#undef GEN_SETTER_STR
#undef GEN_SETTER_U8
#undef GEN_SETTER_U16
#undef GEN_SETTER_U32
#undef GEN_SETTER_U64

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
#define U32(field, defval) config_data.field = defval;
#define U64(field, defval) config_data.field = defval;
        CONFIG_FIELDS(STR, U8, U16, U32, U64);
#undef STR
#undef U8
#undef U16
#undef U32
#undef U64
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
#define U32(field, defval)                                                                         \
    {                                                                                              \
        esp_err_t err = nvs_get_u32(nvs, #field, &config_data.field);                              \
        if (err != ESP_OK) {                                                                       \
            if (err != ESP_ERR_NVS_NOT_FOUND) {                                                    \
                ESP_LOGE(TAG, "LOAD_U32: %s = %u (default), nvs_get_u32 err: %s", #field,          \
                         (unsigned)(defval), esp_err_to_name(err));                                \
            }                                                                                      \
            config_data.field = defval;                                                            \
        }                                                                                          \
    }
#define U64(field, defval)                                                                         \
    {                                                                                              \
        esp_err_t err = nvs_get_u64(nvs, #field, &config_data.field);                              \
        if (err != ESP_OK) {                                                                       \
            if (err != ESP_ERR_NVS_NOT_FOUND) {                                                    \
                ESP_LOGE(TAG, "LOAD_U64: %s = %llu (default), nvs_get_u64 err: %s", #field,        \
                         (unsigned long long)(defval), esp_err_to_name(err));                      \
            }                                                                                      \
            config_data.field = defval;                                                            \
        }                                                                                          \
    }
    CONFIG_FIELDS(STR, U8, U16, U32, U64);
#undef STR
#undef U8
#undef U16
#undef U32
#undef U64
    nvs_close(nvs);
}

const config_t *config_get(void) { return &config_data; }

// The monitor on the TCP port has no authentication, so a password never goes into the log
static bool config_field_is_secret(const char *name) {
    size_t len = strlen(name);
    return len >= 4 && strcmp(name + len - 4, "pass") == 0;
}

void config_manager_print_summary(void) {
    const config_t *cfg = config_get();
    nvs_handle_t nvs;
    bool nvs_open_ok = nvs_open(CONFIG_MANAGER_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK;

    // A field is in NVS only after a setconf; until then the built-in default is used
#define SOURCE(field)                                                                              \
    ((nvs_open_ok && nvs_find_key(nvs, #field, NULL) == ESP_OK) ? "nvs" : "default")
#define PRINT_STR(field, size, defval)                                                             \
    ESP_LOGI(TAG, "| %-16s | %-7s | %-36.36s |", #field, SOURCE(field),                            \
             (config_field_is_secret(#field) && cfg->field[0]) ? "***" : cfg->field);
#define PRINT_NUM(field, fmt, cast)                                                                \
    ESP_LOGI(TAG, "| %-16s | %-7s | " fmt " |", #field, SOURCE(field), (cast)cfg->field);
#define PRINT_U8(field, defval) PRINT_NUM(field, "%-36u", unsigned)
#define PRINT_U16(field, defval) PRINT_NUM(field, "%-36u", unsigned)
#define PRINT_U32(field, defval) PRINT_NUM(field, "%-36u", unsigned)
#define PRINT_U64(field, defval) PRINT_NUM(field, "%-36llu", unsigned long long)
    ESP_LOGI(TAG, "| * CONFIG * (nvs = set at runtime, default = built into the firmware)");
    CONFIG_FIELDS(PRINT_STR, PRINT_U8, PRINT_U16, PRINT_U32, PRINT_U64)
#undef PRINT_STR
#undef PRINT_NUM
#undef PRINT_U8
#undef PRINT_U16
#undef PRINT_U32
#undef PRINT_U64
#undef SOURCE

    if (nvs_open_ok) {
        nvs_close(nvs);
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
#define CONFIG_FIELD_APPLY_SETTER_U32(field, default_val)                                          \
    if (strcmp(item->string, #field) == 0) {                                                       \
        if (item->type == cJSON_Number) {                                                          \
            config_set_##field((uint32_t)item->valuedouble);                                       \
        } else {                                                                                   \
            ESP_LOGW(TAG, "JSON type mismatch for field %s (expected number)", #field);            \
        }                                                                                          \
        return 1;                                                                                  \
    }
#define CONFIG_FIELD_APPLY_SETTER_U64(field, default_val)                                          \
    if (strcmp(item->string, #field) == 0) {                                                       \
        if (item->type == cJSON_Number) {                                                          \
            config_set_##field((uint64_t)item->valuedouble);                                       \
        } else {                                                                                   \
            ESP_LOGW(TAG, "JSON type mismatch for field %s (expected number)", #field);            \
        }                                                                                          \
        return 1;                                                                                  \
    }

static int config_manager_apply_json_item(const cJSON *item) {
    CONFIG_FIELDS(CONFIG_FIELD_APPLY_SETTER_STR, CONFIG_FIELD_APPLY_SETTER_U8,
                  CONFIG_FIELD_APPLY_SETTER_U16, CONFIG_FIELD_APPLY_SETTER_U32,
                  CONFIG_FIELD_APPLY_SETTER_U64)
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
