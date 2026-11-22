#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "tele.h"

#define TAG "cikon:supervisor:tele"

static tele_t tele_registry[CONFIG_SUPERVISOR_MAX_TELE];
static size_t tele_count = 0;
static bool tele_initialized = false;
static SemaphoreHandle_t tele_print_lock = NULL;

void tele_init(void) {
    if (tele_initialized) {
        ESP_LOGW(TAG, "Telemetry system already initialized");
        return;
    }

    tele_print_lock = xSemaphoreCreateMutex();
    if (!tele_print_lock) {
        // Nothing is printed without the lock, so the system stays uninitialized
        ESP_LOGE(TAG, "No memory for the telemetry lock");
        return;
    }

    memset(tele_registry, 0, sizeof(tele_registry));
    tele_count = 0;
    tele_initialized = true;

    ESP_LOGI(TAG, "Telemetry system initialized");
}

void tele_register(const char *tele_id, tele_appender_t fn) {
    if (!tele_initialized) {
        tele_init();
    }

    if (!tele_id || !fn) {
        ESP_LOGE(TAG, "Invalid telemetry registration parameters");
        return;
    }

    if (tele_count >= CONFIG_SUPERVISOR_MAX_TELE) {
        ESP_LOGE(TAG, "Telemetry registry is full (%d sources)", CONFIG_SUPERVISOR_MAX_TELE);
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

// Prints all the telemetry as one JSON object into buf, one source at a time: each source gets a
// small tree of its own that is printed and deleted at once, so no more than the largest source
// is held as a tree. Returns the length, or 0 when it does not fit.
static size_t tele_print_into(char *buf, size_t buf_size) {
    if (!tele_initialized || !buf || buf_size < 3) {
        return 0;
    }

    size_t len = 0;
    for (size_t i = 0; i < tele_count; ++i) {
        tele_appender_t fn = tele_registry[i].fn;
        const char *tele_id = tele_registry[i].tele_id;
        if (!fn || !tele_id) {
            continue;
        }

        cJSON *part = cJSON_CreateObject();
        if (!part) {
            ESP_LOGE(TAG, "No memory for the telemetry of '%s'", tele_id);
            return 0;
        }
        fn(tele_id, part);

        if (part->child) {
            if (!cJSON_PrintPreallocated(part, buf + len, buf_size - len, false)) {
                cJSON_Delete(part);
                return 0;
            }
            // The part is printed as {...}: its opening brace becomes the comma between the parts
            // and its closing brace is overwritten by the next part or by the last brace
            size_t part_len = strlen(buf + len);
            if (len > 0) {
                buf[len] = ',';
            }
            len += part_len - 1;
        }
        cJSON_Delete(part);
    }

    if (len == 0) {
        buf[0] = '{';
        len = 1;
    }
    if (len + 2 > buf_size) {
        return 0;
    }
    buf[len++] = '}';
    buf[len] = '\0';
    return len;
}

// The size of the buffer for the message: what the last message took and a quarter more, so one try
// is enough from the second message on. It grows by doubling when a message does not fit. Only
// changed with the print lock held
static size_t tele_print_size = 512;

// One message is built at a time: a task that comes while another builds waits, so the trees and
// the buffers of two builds are never in memory together. After a second it gives up with NULL
// instead of hanging
char *tele_print_all(void) {
    if (!tele_initialized) {
        ESP_LOGE(TAG, "Telemetry system not initialized");
        return NULL;
    }

    if (xSemaphoreTake(tele_print_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Telemetry is built by another task for too long, skipping");
        return NULL;
    }

    char *buf = NULL;
    int attempt;
    for (attempt = 0; attempt < 5; attempt++) {
        size_t size = tele_print_size;
        // PSRAM first where there is some, as a plain malloc of this size stays in internal RAM
        buf = heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
        if (!buf) {
            ESP_LOGE(TAG, "No memory for a %u B telemetry buffer", (unsigned)size);
            break;
        }

        size_t len = tele_print_into(buf, size);
        if (len > 0) {
            tele_print_size = len + len / 4;
            break;
        }

        free(buf);
        buf = NULL;
        tele_print_size = size * 2;
    }

    xSemaphoreGive(tele_print_lock);

    if (!buf && attempt == 5) {
        ESP_LOGE(TAG, "The telemetry does not fit %u B", (unsigned)tele_print_size);
    }
    return buf;
}
