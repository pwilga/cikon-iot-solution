#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cJSON cJSON;

typedef void (*tele_appender_t)(const char *tele_id, cJSON *json_root);

typedef struct {
    const char *tele_id;
    tele_appender_t fn;
} tele_t;

// Alias for telemetry entry (same structure, used for declaring telemetry groups)
typedef tele_t tele_entry_t;

void tele_init(void);
void tele_register(const char *tele_id, tele_appender_t fn);
void tele_register_group(const tele_entry_t *appenders);
void tele_unregister_group(const tele_entry_t *appenders);
const tele_t *tele_get_registry(size_t *out_count);
const tele_t *tele_find(const char *tele_id);

void tele_append_all(cJSON *json_root);
void tele_append_one(cJSON *json_root, const char *tele_id);

#ifdef __cplusplus
}
#endif
