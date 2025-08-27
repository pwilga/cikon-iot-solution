#ifndef WIFI_H
#define WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char sta_ssid[32];
    char sta_password[32];
    char ap_ssid[32];
    char ap_password[32]; // for future use
} wifi_credentials_t;

void wifi_configure(const wifi_credentials_t *creds);
void wifi_init_sta_mode();
void wifi_init_ap_mode();
/**
 * @brief Completely disables WiFi (STA and AP). If STA is connected, disconnects first.
 * @return esp_err_t result of the last WiFi operation
 */
esp_err_t safe_wifi_stop();

void wifi_unregister_event_handlers();
bool is_wifi_network_connected();
void wifi_log_event_group_bits();
void wifi_get_interface_ip(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif // WIFI_H