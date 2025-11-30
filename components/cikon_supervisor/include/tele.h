#ifndef TELE_H
#define TELE_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tele_appender_t)(const char *tele_id, cJSON *json_root);

typedef struct {
    const char *tele_id;
    tele_appender_t fn;
} tele_t;

void tele_init(void);
void tele_register(const char *tele_id, tele_appender_t fn);
const tele_t *tele_get_registry(size_t *out_count);
const tele_t *tele_find(const char *tele_id);

void tele_append_all(cJSON *json_root);
void tele_append_one(cJSON *json_root, const char *tele_id);

#ifdef __cplusplus
}
#endif

#endif // TELE_H