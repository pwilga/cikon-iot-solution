#pragma once

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*http_json_get_fn_t)(cJSON *out);
typedef void (*http_json_post_fn_t)(const char *json_str);

/**
 * @brief Register a JSON GET endpoint.
 * @param uri  URI path (e.g. "/tele").
 * @param fn   Callback that fills the response JSON object.
 */
void http_register_json_get(const char *uri, http_json_get_fn_t fn);

/**
 * @brief Register a JSON POST endpoint.
 * @param uri  URI path (e.g. "/cmnd").
 * @param fn   Callback that receives the raw request body as a JSON string.
 */
void http_register_json_post(const char *uri, http_json_post_fn_t fn);

void http_init(void);
void http_shutdown(void);

#ifdef __cplusplus
}
#endif
