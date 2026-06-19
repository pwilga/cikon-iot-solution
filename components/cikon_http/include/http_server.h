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
    uint8_t  max_open_sockets;
    bool     secure;
} http_config_t;

typedef void (*http_json_get_fn_t)(cJSON *json);
typedef void (*http_json_post_fn_t)(const char *json_str);

void http_init(const http_config_t *cfg);
void http_shutdown(void);
void http_register_json_get(const char *uri, http_json_get_fn_t fn);
void http_register_json_post(const char *uri, http_json_post_fn_t fn);

#ifdef __cplusplus
}
#endif
