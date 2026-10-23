#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *sta_ssid;
    const char *sta_password;
    const char *ap_ssid;
    const char *ap_password;
} wifi_credentials_t;

/**
 * @brief Start WiFi in STA mode and keep reconnecting to the configured network.
 * Network interfaces are created once and never destroyed; the AP is added next to STA on demand.
 */
esp_err_t wifi_start(const wifi_credentials_t *creds);

/**
 * @brief Connect STA to the given network, dropping the current one first. No restart needed.
 * The strings must stay valid while WiFi runs (only the pointers are kept).
 */
esp_err_t wifi_sta_connect(const char *ssid, const char *password);

/**
 * @brief Blocking scan for nearby networks (~3 s). Fills up to max records and returns how many:
 * one per network name (the strongest access point), hidden networks left out.
 * STA connection attempts pause during the scan; a connected STA stays connected.
 */
uint16_t wifi_scan(wifi_ap_record_t *records, uint16_t max);

/**
 * @brief Bring up the access point next to the running STA (WIFI_MODE_APSTA).
 * STA stays connected and services keep running on both interfaces.
 */
void wifi_ap_enable(void);

/**
 * @brief Take the access point down, back to STA only. STA stays connected.
 */
void wifi_ap_disable(void);

/**
 * @brief Completely shuts down WiFi subsystem (stops WiFi, unregisters handlers,
 * deinitializes driver and event loop). Use when disabling WiFi completely (e.g. switching to
 * Zigbee on C6).
 */
void wifi_shutdown();

void wifi_unregister_event_handlers();
void wifi_log_event_group_bits();
void wifi_get_interface_ip(char *buf, size_t buflen);
int8_t wifi_get_rssi(void);
bool wifi_get_ssid(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif // WIFI_H