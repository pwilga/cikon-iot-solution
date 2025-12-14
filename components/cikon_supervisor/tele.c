#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "tele.h"

#define TAG "cikon:supervisor:tele"

static tele_t tele_registry[CONFIG_MAX_TELE];
static size_t tele_count = 0;
static bool tele_initialized = false;

void tele_init(void) {
    if (tele_initialized) {
        ESP_LOGW(TAG, "Telemetry system already initialized");
        return;
    }

    memset(tele_registry, 0, sizeof(tele_registry));
    tele_count = 0;
    tele_initialized = true;

    ESP_LOGI(TAG, "Telemetry system initialized: %zu sources", tele_count);
}

void tele_register(const char *tele_id, tele_appender_t fn) {
    if (!tele_initialized) {
        tele_init();
    }

    if (!tele_id || !fn) {
        ESP_LOGE(TAG, "Invalid telemetry registration parameters");
        return;
    }

    if (tele_count >= CONFIG_MAX_TELE) {
        ESP_LOGE(TAG, "Telemetry registry is full (%d sources)", CONFIG_MAX_TELE);
        return;
    }

    for (size_t i = 0; i < tele_count; ++i) {
        if (strcmp(tele_registry[i].tele_id, tele_id) == 0) {
            ESP_LOGW(TAG, "Telemetry '%s' already registered, skipping", tele_id);
            return;
        }
    }

    tele_registry[tele_count].tele_id = tele_id;
    tele_registry[tele_count].fn = fn;
    tele_count++;
}

void tele_register_group(const tele_entry_t *appenders) {
    if (!appenders) {
        ESP_LOGE(TAG, "Invalid telemetry group parameters");
        return;
    }

    for (size_t i = 0; appenders[i].tele_id != NULL; i++) {
        tele_register(appenders[i].tele_id, appenders[i].fn);
    }
}

void tele_unregister_group(const tele_entry_t *appenders) {
    if (!appenders) {
        ESP_LOGE(TAG, "Invalid telemetry group parameters");
        return;
    }

    for (size_t i = 0; appenders[i].tele_id != NULL; i++) {
        const char *tele_id = appenders[i].tele_id;

        for (size_t j = 0; j < tele_count; j++) {
            if (strcmp(tele_registry[j].tele_id, tele_id) == 0) {
                // Shift all appenders after this one down
                for (size_t k = j; k < tele_count - 1; k++) {
                    tele_registry[k] = tele_registry[k + 1];
                }
                tele_count--;
                ESP_LOGI(TAG, "Telemetry '%s' unregistered", tele_id);
                break;
            }
        }
    }
}

const tele_t *tele_get_registry(size_t *out_count) {
    if (out_count) {
        *out_count = tele_initialized ? tele_count : 0;
    }
    if (!tele_initialized) {
        return NULL;
    }
    return tele_registry;
}

const tele_t *tele_find(const char *tele_id) {
    if (!tele_initialized) {
        ESP_LOGE(TAG, "Telemetry system not initialized");
        return NULL;
    }
    if (!tele_id) {
        ESP_LOGE(TAG, "Telemetry ID is NULL");
        return NULL;
    }

    for (size_t i = 0; i < tele_count; ++i) {
        if (strcmp(tele_registry[i].tele_id, tele_id) == 0) {
            return &tele_registry[i];
        }
    }

    return NULL;
}

void tele_append_all(cJSON *json_root) {
    if (!tele_initialized) {
        ESP_LOGE(TAG, "Telemetry system not initialized");
        return;
    }
    if (!json_root) {
        ESP_LOGW(TAG, "Invalid JSON root; skipping telemetry append");
        return;
    }

    for (size_t i = 0; i < tele_count; ++i) {
        tele_appender_t fn = tele_registry[i].fn;
        const char *tele_id = tele_registry[i].tele_id;
        if (fn && tele_id) {
            fn(tele_id, json_root);
        }
    }
}

void tele_append_one(cJSON *json_root, const char *tele_id) {
    if (!tele_initialized) {
        ESP_LOGE(TAG, "Telemetry system not initialized");
        return;
    }
    if (!json_root) {
        ESP_LOGW(TAG, "Invalid JSON root; skipping telemetry append");
        return;
    }
    if (!tele_id) {
        ESP_LOGE(TAG, "Telemetry ID is NULL");
        return;
    }

    const tele_t *t = tele_find(tele_id);
    if (!t) {
        ESP_LOGW(TAG, "Unknown telemetry: %s", tele_id);
        return;
    }
    if (t->fn) {
        t->fn(t->tele_id, json_root);
    }
}
