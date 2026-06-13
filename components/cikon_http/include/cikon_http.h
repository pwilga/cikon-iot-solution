#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register a URI handler.
 *
 * Safe to call before cikon_http_init() — queued and applied on start.
 * Safe to call after cikon_http_init() — registered immediately.
 * @param uri  Pointer to a statically allocated httpd_uri_t.
 */
esp_err_t http_register_uri(const httpd_uri_t *uri);

void http_init(void);
void http_shutdown(void);

#ifdef __cplusplus
}
#endif
