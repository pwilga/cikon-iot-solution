#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// NOLINTNEXTLINE(readability-identifier-naming)
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

// All the telemetry as one JSON string on the heap, to be freed by the caller. NULL when there is
// no memory for it
char *tele_print_all(void);

#ifdef __cplusplus
}
#endif
