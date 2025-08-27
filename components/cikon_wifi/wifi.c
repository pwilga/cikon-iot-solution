#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "sdkconfig.h"

#include "wifi.h"

#define WIFI_STA_CONNECTED_BIT BIT0
#define WIFI_AP_STARTED_BIT BIT1

#define TAG "cikon-wifi"
#define TAG_STA TAG "-sta"
#define TAG_AP TAG "-ap"

static wifi_credentials_t wifi_creds;
static esp_netif_t *sta_netif, *ap_netif;

static bool ignore_sta_disconnect_event = false;
static bool ap_has_client = false;

static TaskHandle_t wifi_sta_connection_task_handle = NULL;
static TaskHandle_t wifi_ap_task_handle = NULL;

volatile static uint32_t ap_seconds_without_clients = 0;

static portMUX_TYPE wifi_sta_task_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE wifi_ap_timeout_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_event_handler_instance_t instance_any_id;
static esp_event_handler_instance_t instance_got_ip;

static EventGroupHandle_t wifi_event_group = NULL;

const char *wifi_disconnect_reason_str(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:
        return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE:
        return "ASSOC_EXPIRE";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief WiFi STA connection retry task with exponential backoff.
 *
 * After each burst of connection attempts, the interval before the next burst is multiplied
 * (doubled) to reduce network and power usage if the network is unavailable for a long time. This
 * prevents constant retries and allows for quick reconnection if the network returns soon, while
 * spacing out attempts over time if the outage persists.
 *
 * @param args Unused.
 */
static void wifi_sta_connection_task(void *args) {

    int interval = CONFIG_WIFI_STA_RETRY_INITIAL_MS;

    while (1) {
        bool connected = false;
        for (int i = 0; i < CONFIG_WIFI_STA_BURST_RETRY_COUNT; ++i) {

            ESP_LOGI(TAG_STA, "WIFI Connection retry (%d/%d).", i + 1,
                     CONFIG_WIFI_STA_BURST_RETRY_COUNT);
            ESP_ERROR_CHECK(esp_wifi_disconnect());
            ESP_ERROR_CHECK(esp_wifi_connect());

            EventBits_t bits =
                xEventGroupWaitBits(wifi_event_group, WIFI_STA_CONNECTED_BIT, pdFALSE, pdFALSE,
                                    pdMS_TO_TICKS(CONFIG_WIFI_STA_BURST_RETRY_DELAY_MS));

            if (bits & WIFI_STA_CONNECTED_BIT) {
                connected = true;
                break;
            }
        }

        if (connected) {
            break;
        }

        ESP_LOGI(TAG_STA, "Waiting %.2f s before next group of connection attempts",
                 interval / 1000.0);
        vTaskDelay(pdMS_TO_TICKS(interval));
        interval *= 2;
        if (interval > CONFIG_WIFI_STA_RETRY_MAX_MS)
            interval = CONFIG_WIFI_STA_RETRY_MAX_MS;
    }
    wifi_sta_connection_task_handle = NULL;
    vTaskDelete(NULL);
}

void wifi_sta_connection_task_ensure_running(void) {

    taskENTER_CRITICAL(&wifi_sta_task_mux);
    if (wifi_sta_connection_task_handle == NULL ||
        eTaskGetState(wifi_sta_connection_task_handle) == eDeleted) {
        xTaskCreate(wifi_sta_connection_task, "sta_con", CONFIG_WIFI_STA_CONNECTION_TASK_STACK_SIZE,
                    NULL, CONFIG_WIFI_STA_CONNECTION_TASK_PRIORITY,
                    &wifi_sta_connection_task_handle);
    }
    taskEXIT_CRITICAL(&wifi_sta_task_mux);
}

static void wifi_ap_timeout_task(void *args) {

    const int timeout_seconds = CONFIG_WIFI_AP_INACTIVITY_TIMEOUT_MINUTES * 60;

    while (xEventGroupGetBits(wifi_event_group) & WIFI_AP_STARTED_BIT) {
        if (ap_has_client) {

            taskENTER_CRITICAL(&wifi_ap_timeout_mux);
            ap_seconds_without_clients = 0;
            taskEXIT_CRITICAL(&wifi_ap_timeout_mux);

        } else {

            taskENTER_CRITICAL(&wifi_ap_timeout_mux);
            ap_seconds_without_clients++;
            taskEXIT_CRITICAL(&wifi_ap_timeout_mux);

            if (ap_seconds_without_clients >= timeout_seconds) {
                ESP_LOGI(
                    TAG_AP,
                    "🚦 AP inactivity timeout: no clients for %d minute(s), disabling Access Point "
                    "(AP).",
                    CONFIG_WIFI_AP_INACTIVITY_TIMEOUT_MINUTES);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    wifi_mode_t mode;

    if (esp_wifi_get_mode(&mode) != ESP_OK || mode != WIFI_MODE_STA) {
        ESP_LOGI(TAG_AP, "Switching to STA mode.");
        wifi_init_sta_mode();
    } else {
        ESP_LOGI(TAG_AP, "Already in STA mode, no need to switch.");
    }

    wifi_ap_task_handle = NULL;
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data) {

    if (event_base == WIFI_EVENT) {

        switch (event_id) {

        case WIFI_EVENT_STA_DISCONNECTED:

            xEventGroupClearBits(wifi_event_group, WIFI_STA_CONNECTED_BIT);

            wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGI(TAG_STA, "Disconnected, reason: %d (%s)", disconn->reason,
                     wifi_disconnect_reason_str(disconn->reason));

            if (ignore_sta_disconnect_event) {
                // ESP_LOGW(TAG_STA,
                //          "Ignoring disconnect event due to ongoing shutdown or mode change.");
                ignore_sta_disconnect_event = false;
                return;
            }

            wifi_sta_connection_task_ensure_running();

            break;

        case WIFI_EVENT_AP_START:
            xEventGroupSetBits(wifi_event_group, WIFI_AP_STARTED_BIT);
            ap_has_client = false;

            if (wifi_ap_task_handle != NULL) {
                eTaskState state = eTaskGetState(wifi_ap_task_handle);
                if (state != eDeleted && state != eInvalid) {
                    // ESP_LOGW(TAG_AP, "AP timeout task already running (state: %d), ignoring.",
                    //          state);
                    break;
                }
            }

            xTaskCreate(wifi_ap_timeout_task, "ap_timeout", CONFIG_WIFI_AP_TIMEOUT_TASK_STACK_SIZE,
                        NULL, CONFIG_WIFI_AP_TIMEOUT_TASK_PRIORITY, &wifi_ap_task_handle);
            break;

        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG_AP, "Access Point (AP) has been stopped.");
            xEventGroupClearBits(wifi_event_group, WIFI_AP_STARTED_BIT);
            break;

        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG_AP, "A client has connected to the Access Point (AP).");
            ap_has_client = true;
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG_AP, "A client has disconnected from the Access Point (AP).");
            ap_has_client = false;
            break;

        default:
            break;
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG_STA, "Successfully connected to WiFi network (SSID: %s).",
                 wifi_creds.sta_ssid);

        xEventGroupSetBits(wifi_event_group, WIFI_STA_CONNECTED_BIT);
    }
}

void wifi_unregister_event_handlers() {
    /* Just to avoid errors when esp32 close wifi connection */
    ESP_ERROR_CHECK(
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));

    ESP_ERROR_CHECK(
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
}

bool is_wifi_network_connected(void) {
    return xEventGroupGetBits(wifi_event_group) & (WIFI_STA_CONNECTED_BIT | WIFI_AP_STARTED_BIT);
}

void wifi_configure(const wifi_credentials_t *creds) {

    if (creds) {
        wifi_creds = *creds;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    static StaticEventGroup_t event_group_storage;

    if (wifi_event_group == NULL) {
        wifi_event_group = xEventGroupCreateStatic(&event_group_storage);
        // wifi_event_group = xEventGroupCreate();
    }

    if (!wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create WiFi event group!");
    }
}

void wifi_init_ap_mode() {

    ignore_sta_disconnect_event = true;
    ESP_ERROR_CHECK(safe_wifi_stop());

    wifi_config_t ap_config = {.ap = {.ssid_len = 0,
                                      .password = "",
                                      .channel = 1,
                                      .max_connection = 1,
                                      .authmode = WIFI_AUTH_OPEN}};

    strncpy((char *)ap_config.ap.ssid, wifi_creds.ap_ssid, sizeof(ap_config.ap.ssid) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    ap_netif = esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_wifi_start());
}

void wifi_init_sta_mode() {

    if (sta_netif != NULL) {
        ignore_sta_disconnect_event = true;
    } else {
        ignore_sta_disconnect_event = false;
    }

    ESP_ERROR_CHECK(safe_wifi_stop());

    wifi_config_t sta_config = {0};

    strncpy((char *)sta_config.sta.ssid, wifi_creds.sta_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, wifi_creds.sta_password,
            sizeof(sta_config.sta.password) - 1);

    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    sta_netif = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_sta_connection_task_ensure_running();
}

esp_err_t safe_wifi_stop() {

    // Atomically (thread-safe) detach the task handle from the global variable,
    // so that no other code can use or delete it at the same time (prevents race conditions).
    // vTaskDelete is called outside the critical section, because it may block or take time.
    //
    // This pattern should be used whenever a task handle can be accessed or deleted
    // from multiple contexts (e.g. event handlers, timers, other tasks).
    //
    // For simple, single-context code (e.g. only one task manages the handle),
    // a critical section may not be needed.
    TaskHandle_t sta_tmp_handle = NULL;

    taskENTER_CRITICAL(&wifi_sta_task_mux);
    sta_tmp_handle = wifi_sta_connection_task_handle;
    wifi_sta_connection_task_handle = NULL;
    taskEXIT_CRITICAL(&wifi_sta_task_mux);

    if (sta_tmp_handle != NULL) {
        vTaskDelete(sta_tmp_handle);
    }

    taskENTER_CRITICAL(&wifi_ap_timeout_mux);
    ap_seconds_without_clients = 0;
    taskEXIT_CRITICAL(&wifi_ap_timeout_mux);

    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_stop();

    xEventGroupClearBits(wifi_event_group, WIFI_STA_CONNECTED_BIT | WIFI_AP_STARTED_BIT);

    if (sta_netif) {
        esp_netif_destroy(sta_netif);
        sta_netif = NULL;
    }

    if (ap_netif) {
        esp_netif_destroy(ap_netif);
        ap_netif = NULL;
    }

    return err;
}

void wifi_get_interface_ip(char *buf, size_t buflen) {

    esp_netif_ip_info_t ip_info;
    esp_netif_t *active = NULL;

    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        active = sta_netif;
    } else if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK &&
               ip_info.ip.addr != 0) {
        active = ap_netif;
    }
    if (active) {
        snprintf(buf, buflen, "%d.%d.%d.%d", esp_ip4_addr1(&ip_info.ip), esp_ip4_addr2(&ip_info.ip),
                 esp_ip4_addr3(&ip_info.ip), esp_ip4_addr4(&ip_info.ip));
    } else {
        snprintf(buf, buflen, "0.0.0.0");
    }
}

void wifi_log_event_group_bits(void) {

    if (!wifi_event_group)
        return;

    EventBits_t bits = xEventGroupGetBits(wifi_event_group);
    ESP_LOGI(TAG, "WiFi bits: %s%s", (bits & WIFI_STA_CONNECTED_BIT) ? "STA " : "",
             (bits & WIFI_AP_STARTED_BIT) ? "AP " : "");
}
