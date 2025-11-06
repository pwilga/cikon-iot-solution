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
 * - Basic HTTP authentication (Basic Auth) is required for all operations. Credentials sent by the
 * client are compared to the reference value stored securely in NVS and accessed via config_manager
 * (config_get()->http_auth).
 *
 * Limitations & Notes:
 * - This code is experimental and not recommended for production use without further review and
 * testing.
 * - No HTTP keep-alive: each request is handled in a new connection for simplicity and to avoid RAM
 * leaks.
 * - Only one connection is allowed at a time.
 * - The server is automatically shut down after a configurable period of inactivity.
 *
 * Usage:
 * - Use https_init() to start the server.
 * - Use https_shutdown() to request a shutdown.
 * - All sensitive operations (e.g., password changes) should be performed via HTTPS POST requests
 * with Basic Auth.
 */

#ifndef HTTPS_SERVER_H
#define HTTPS_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

void https_configure(const char *http_auth);

void https_init(void);
void https_shutdown(void);

void https_server_task(void *args);

#ifdef __cplusplus
}
#endif

#endif // HTTPS_SERVER_H
