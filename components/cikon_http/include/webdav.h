#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

void http_webdav_init(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
