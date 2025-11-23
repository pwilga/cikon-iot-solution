/**
 * @file https_server.h
 * @brief Experimental HTTPS server interface for ESP32.
 *
 * This module provides a minimal, highly experimental HTTPS server for ESP32, intended for secure
 * operations such as password transmission, configuration changes, and other sensitive actions. The
 * server is designed for single-connection use, with aggressive resource management and automatic
 * shutdown after inactivity.
 *
 * Security features:
 * - All communication is encrypted using TLS (HTTPS).
 * - Basic HTTP authentication (Basic Auth) is optional. If http_auth is NULL or empty string,
 *   authentication is disabled. When enabled, credentials sent by the client are compared to the
 *   reference value provided via https_configure().
 *
 * Endpoint Configuration:
 * - Endpoints are registered via https_configure() using https_endpoint_config_t array.
 * - Array must be terminated with a sentinel entry where .uri = NULL.
 * - POST endpoints use json_cmnd callback, GET endpoints use json_tele callback.
 * - Each endpoint can have custom URI, method, and associated callback.
 *
 * Limitations & Notes:
 * - This code is experimental and not recommended for production use without further review and
 * testing.
 * - No HTTP keep-alive: each request is handled in a new connection for simplicity and to avoid RAM
 * leaks.
 * - Maximum number of open sockets is configurable (default: 1).
 * - The server is automatically shut down and restarted after a configurable period of inactivity.
 *
 * Usage:
 * - Call https_configure() with endpoints array and optional http_auth string.
 * - Use https_init() to start the server.
 * - Use https_shutdown() to request a shutdown (waits for graceful task termination).
 */

#ifndef HTTPS_SERVER_H
#define HTTPS_SERVER_H

#include "cJSON.h"
#include <esp_https_server.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*https_json_cmnd_t)(const char *json_str);
typedef void (*https_json_tele_t)(cJSON *json);

typedef struct {
    const char *uri;
    httpd_method_t method;
    https_json_cmnd_t json_cmnd; // POST
    https_json_tele_t json_tele; // GET
} https_endpoint_config_t;

void https_configure(const https_endpoint_config_t *endpoints, const char *http_auth);

void https_init(void);
void https_shutdown(void);
void https_server_task(void *args);

#ifdef __cplusplus
}
#endif

#endif // HTTPS_SERVER_H
