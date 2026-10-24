#pragma once

#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t port;
    uint16_t ctrl_port;
    uint8_t max_open_sockets;
    bool secure;
} http_config_t;

typedef void (*http_json_get_fn_t)(cJSON *json);
typedef void (*http_json_post_fn_t)(const char *json_str);

void http_init(const http_config_t *cfg);
void http_shutdown(void);

// Can be called before http_init(): endpoints are kept and registered on every server start.
// uri must be a string literal (only the pointer is kept)
void http_register_json_get(const char *uri, http_json_get_fn_t fn);
void http_register_json_post(const char *uri, http_json_post_fn_t fn);

// Missing files answer with 302 to location (path or full URL) instead of 404, NULL brings 404
// back. Can be called before http_init(), survives server restarts. Only the pointer is kept
void http_set_not_found_redirect(const char *location);

#ifdef __cplusplus
}
#endif
