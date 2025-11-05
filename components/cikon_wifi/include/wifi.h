#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *sta_ssid;
    const char *sta_password;
    const char *ap_ssid;
    const char *ap_password;
} wifi_credentials_t;

// Callback called when AP timeout occurs and system wants to switch to STA
// Upper layers can use this to cleanup services before switch
typedef void (*wifi_ap_timeout_callback_t)(void);

void wifi_set_ap_timeout_callback(wifi_ap_timeout_callback_t cb);

void wifi_configure(const wifi_credentials_t *creds);
void wifi_init_sta_mode();
void wifi_init_ap_mode();
/**
 * @brief Completely disables WiFi (STA and AP). If STA is connected, disconnects first.
 * @return esp_err_t result of the last WiFi operation
 */
esp_err_t safe_wifi_stop();

/**
 * @brief Completely shuts down WiFi subsystem (stops WiFi, unregisters handlers,
 * deinitializes driver and event loop). Use when disabling WiFi completely (e.g. switching to
 * Zigbee on C6).
 */
void wifi_shutdown();

void wifi_unregister_event_handlers();
bool is_wifi_network_connected();
void wifi_log_event_group_bits();
void wifi_get_interface_ip(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif // WIFI_H