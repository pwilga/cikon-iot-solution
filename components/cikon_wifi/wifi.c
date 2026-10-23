#include <inttypes.h>
#include <string.h>
#include <sys/param.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/idf_additions.h"

#include "wifi.h"

#define WIFI_STA_CONNECTED_BIT BIT0
#define WIFI_AP_STARTED_BIT BIT1
#define WIFI_STOPPED_BIT BIT2
#define WIFI_AP_CLIENT_CONNECTED_BIT BIT3
#define WIFI_SHUTDOWN_INITIATED_BIT BIT4

#define TAG "cikon:wifi"
#define TAG_STA TAG ":sta"
#define TAG_AP TAG ":ap"

static wifi_credentials_t wifi_creds = {NULL};
static esp_netif_t *sta_netif = NULL, *ap_netif = NULL;

static esp_timer_handle_t sta_reconnect_timer = NULL;
static uint8_t sta_reconnect_attempt = 0; // attempts made in the current burst
static uint32_t sta_reconnect_backoff_ms = CONFIG_WIFI_STA_RETRY_INITIAL_MS;
static bool sta_creds_changed = false; // wifi_creds changed, not handed to the driver yet
static bool sta_scanning = false;      // wifi_scan() runs, no connection attempts meanwhile

static esp_timer_handle_t ap_idle_timer = NULL;
static bool ap_auto_allowed = true; // STA hasn't had an IP since boot and the AP wasn't opened yet
static bool ap_unused_since_auto_open = false; // AP opened on its own, nobody connected yet
static bool ap_closing_after_sta_ip = false;   // STA got its IP, the AP closes for good

static esp_event_handler_instance_t instance_any_id = NULL;
static esp_event_handler_instance_t instance_got_ip = NULL;

static EventGroupHandle_t wifi_event_group = NULL;

// Timer callback: only kicks off one attempt. esp_wifi_connect() doesn't wait - it queues the
// request to the WiFi task and returns; the result comes back as IP_EVENT_STA_GOT_IP or
// WIFI_EVENT_STA_DISCONNECTED, and the latter schedules the next attempt. If the request itself
// is refused no event follows, so re-arm the timer to keep the chain going
static void wifi_sta_reconnect(void *arg) {

    // New credentials go to the driver here, right before connecting: this is the only place that
    // starts a connection, so STA can't be in the middle of one (the driver refuses set_config
    // then)
    if (sta_creds_changed) {
        wifi_config_t sta_config = {0};

        strncpy((char *)sta_config.sta.ssid, wifi_creds.sta_ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, wifi_creds.sta_password,
                sizeof(sta_config.sta.password) - 1);

        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        if (esp_wifi_set_config(WIFI_IF_STA, &sta_config) != ESP_OK) {
            esp_timer_start_once(sta_reconnect_timer,
                                 (uint64_t)CONFIG_WIFI_STA_BURST_RETRY_DELAY_MS * 1000);
            return;
        }
        sta_creds_changed = false;
    }

    if (esp_wifi_connect() != ESP_OK) {
        esp_timer_start_once(sta_reconnect_timer,
                             (uint64_t)CONFIG_WIFI_STA_BURST_RETRY_DELAY_MS * 1000);
    }
}

// Runs in the event handler, not in the timer: counting, logging and picking the delay. Within a
// burst attempts are BURST_RETRY_DELAY apart, after a burst comes a pause that doubles up to
// RETRY_MAX before the next burst
static void wifi_sta_schedule_reconnect(void) {

    esp_timer_stop(sta_reconnect_timer);

    // The driver refuses to scan while STA connects - wifi_scan() resumes the attempts itself
    if (sta_scanning) {
        return;
    }

    // First attempt since boot failed and STA never had an IP: open the AP once, so the device can
    // be reached to fix its credentials. Later failures don't open it - a router off for a day
    // shouldn't keep an AP up all day
    if (ap_auto_allowed && sta_reconnect_attempt > 0) {
        ap_auto_allowed = false;
        if (!(xEventGroupGetBits(wifi_event_group) & WIFI_AP_STARTED_BIT)) {
            ESP_LOGW(TAG_AP, "Opening AP - no STA connection since boot");
            ap_unused_since_auto_open = true;
            wifi_ap_enable();
        }
    }

    // Fresh disconnect: first attempt right away, no need to bounce through the timer
    if (sta_reconnect_attempt == 0) {
        sta_reconnect_attempt = 1;
        ESP_LOGW(TAG_STA, "Connecting to '%s' (1/%d)", wifi_creds.sta_ssid,
                 CONFIG_WIFI_STA_BURST_RETRY_COUNT);
        wifi_sta_reconnect(NULL);
        return;
    }

    // Burst used up: pause, the timer then starts the next burst with attempt 1
    if (sta_reconnect_attempt >= CONFIG_WIFI_STA_BURST_RETRY_COUNT) {
        uint32_t pause_ms = sta_reconnect_backoff_ms;
        sta_reconnect_backoff_ms = MIN(sta_reconnect_backoff_ms * 2, CONFIG_WIFI_STA_RETRY_MAX_MS);
        sta_reconnect_attempt = 1;
        ESP_LOGW(TAG_STA,
                 "Failed to connect to '%s' after %d retries, trying again in %" PRIu32 "s",
                 wifi_creds.sta_ssid, CONFIG_WIFI_STA_BURST_RETRY_COUNT, pause_ms / 1000);
        esp_timer_start_once(sta_reconnect_timer, (uint64_t)pause_ms * 1000);
        return;
    }

    // Within the burst
    sta_reconnect_attempt++;
    ESP_LOGW(TAG_STA, "Retrying connection to '%s' (%d/%d) in %ds", wifi_creds.sta_ssid,
             sta_reconnect_attempt, CONFIG_WIFI_STA_BURST_RETRY_COUNT,
             CONFIG_WIFI_STA_BURST_RETRY_DELAY_MS / 1000);
    esp_timer_start_once(sta_reconnect_timer,
                         (uint64_t)CONFIG_WIFI_STA_BURST_RETRY_DELAY_MS * 1000);
}

static void wifi_sta_reconnect_reset(void) {
    if (sta_reconnect_timer) {
        esp_timer_stop(sta_reconnect_timer);
    }
    sta_reconnect_attempt = 0;
    sta_reconnect_backoff_ms = CONFIG_WIFI_STA_RETRY_INITIAL_MS;
}

// Once STA gets its IP the AP only has to last until the portal has seen the result
#define WIFI_AP_TIMEOUT_AFTER_STA_IP_S 20

// (Re)start the countdown to closing the AP. It runs only while STA has its IP - without it the AP
// is the only way to reach the device, so it stays. Connected clients don't extend it
static void wifi_ap_idle_countdown_start(uint32_t timeout_s, const char *reason) {
    esp_timer_stop(ap_idle_timer);
    esp_timer_start_once(ap_idle_timer, (uint64_t)timeout_s * 1000000);
    ESP_LOGI(TAG_AP, "AP closes in %" PRIu32 "s - %s", timeout_s, reason);
}

// Timer callback: the countdown ran out. Just a mode change, STA and services are untouched
static void wifi_ap_idle_timeout(void *arg) {
    ESP_LOGI(TAG_AP, "AP countdown ran out, closing AP");
    wifi_ap_disable();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data) {

    if (event_base == WIFI_EVENT) {

        switch (event_id) {

        case WIFI_EVENT_STA_DISCONNECTED:

            xEventGroupClearBits(wifi_event_group, WIFI_STA_CONNECTED_BIT);
            // An unused auto-opened AP keeps its own limit - failed attempts mustn't stop it
            if (!ap_unused_since_auto_open && esp_timer_is_active(ap_idle_timer)) {
                esp_timer_stop(ap_idle_timer);
                ESP_LOGI(TAG_AP, "AP stays on - STA lost its network");
            }

            wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
            // SSID from the event, not wifi_creds - after wifi_sta_connect() those already hold the
            // new network
            ESP_LOGW(TAG_STA, "Disconnected from '%.*s', reason: %d", disconn->ssid_len,
                     disconn->ssid, disconn->reason);

            wifi_sta_schedule_reconnect();

            break;

        case WIFI_EVENT_AP_START:
            xEventGroupSetBits(wifi_event_group, WIFI_AP_STARTED_BIT);
            xEventGroupClearBits(wifi_event_group, WIFI_AP_CLIENT_CONNECTED_BIT);
            bool sta_has_ip = xEventGroupGetBits(wifi_event_group) & WIFI_STA_CONNECTED_BIT;
            if (sta_has_ip || ap_unused_since_auto_open) {
                wifi_ap_idle_countdown_start(CONFIG_WIFI_AP_INACTIVITY_TIMEOUT_MINUTES * 60,
                                             sta_has_ip ? "STA is connected"
                                                        : "auto_allowed timeout");
            } else {
                ESP_LOGI(TAG_AP, "AP stays on until STA gets an IP");
            }
            break;

        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG_AP, "Access Point has been stopped.");
            esp_timer_stop(ap_idle_timer);
            ap_unused_since_auto_open = false;
            ap_closing_after_sta_ip = false;
            // No AP, no clients - STADISCONNECTED may come after the mode change and miss the bit
            xEventGroupClearBits(wifi_event_group,
                                 WIFI_AP_STARTED_BIT | WIFI_AP_CLIENT_CONNECTED_BIT);
            xEventGroupSetBits(wifi_event_group, WIFI_STOPPED_BIT);
            break;

        case WIFI_EVENT_STA_STOP:
            ESP_LOGI(TAG_STA, "Station (STA) has been stopped.");
            xEventGroupSetBits(wifi_event_group, WIFI_STOPPED_BIT);
            break;

        case WIFI_EVENT_AP_STACONNECTED:
            xEventGroupSetBits(wifi_event_group, WIFI_AP_CLIENT_CONNECTED_BIT);
            // Someone uses the AP: the countdown waits until the last client leaves. An auto-opened
            // AP becomes like one turned on by hand. Closing after STA got its IP goes on
            if (esp_timer_is_active(ap_idle_timer) && !ap_closing_after_sta_ip) {
                ap_unused_since_auto_open = false;
                esp_timer_stop(ap_idle_timer);
                ESP_LOGI(TAG_AP, "AP kept on - client connected");
            }
            break;

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_sta_list_t clients;
            if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num == 0) {
                xEventGroupClearBits(wifi_event_group, WIFI_AP_CLIENT_CONNECTED_BIT);
                if ((xEventGroupGetBits(wifi_event_group) & WIFI_STA_CONNECTED_BIT) &&
                    !ap_closing_after_sta_ip) {
                    wifi_ap_idle_countdown_start(CONFIG_WIFI_AP_INACTIVITY_TIMEOUT_MINUTES * 60,
                                                 "last client left");
                }
            }
            break;
        }

        default:
            break;
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG_STA, "Successfully connected to WiFi network (SSID: %s).",
                 wifi_creds.sta_ssid);

        xEventGroupSetBits(wifi_event_group, WIFI_STA_CONNECTED_BIT);
        wifi_sta_reconnect_reset();
        ap_auto_allowed = false;

        EventBits_t bits = xEventGroupGetBits(wifi_event_group);
        if (bits & WIFI_AP_STARTED_BIT) {
            // Nobody on the AP waits for the result - close it right away
            if (!(bits & WIFI_AP_CLIENT_CONNECTED_BIT)) {
                ESP_LOGI(TAG_AP, "Closing AP - STA got an IP, no client");
                wifi_ap_disable();
            } else {
                ap_closing_after_sta_ip = true;
                wifi_ap_idle_countdown_start(WIFI_AP_TIMEOUT_AFTER_STA_IP_S, "STA got an IP");
            }
        }
    }
}

void wifi_unregister_event_handlers() {

    if (instance_got_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
        instance_got_ip = NULL;
    }

    if (instance_any_id) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
        instance_any_id = NULL;
    }
}

static esp_err_t wifi_stop(void) {

    // Check if WiFi driver is running (handle both netif and minimal mode)
    wifi_mode_t mode;
    esp_err_t mode_err = esp_wifi_get_mode(&mode);

    // If WiFi not initialized or not started, nothing to stop
    if (mode_err == ESP_ERR_WIFI_NOT_INIT || mode_err == ESP_ERR_WIFI_NOT_STARTED) {
        return ESP_OK;
    }

    // If WiFi was started by cikon_wifi (netif exists), do full cleanup
    bool has_netif = (sta_netif != NULL || ap_netif != NULL);

    if (has_netif) {
        // No reconnect attempt or AP timeout may fire while WiFi is being stopped
        wifi_sta_reconnect_reset();
        esp_timer_stop(ap_idle_timer);

        // Clear the STOPPED bit before calling esp_wifi_stop()
        xEventGroupClearBits(wifi_event_group, WIFI_STOPPED_BIT);
    }

    // Disconnect all AP clients before stopping (MAC 0 = disconnect all)
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        esp_wifi_deauth_sta(0);
    }

    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_stop();

    // Wait for stop confirmation (different for netif vs minimal mode)
    if (has_netif) {
        // Check if shutdown is in progress - event handlers are already unregistered
        // so WIFI_STOPPED_BIT will never be set by events
        EventBits_t shutdown_bits = xEventGroupGetBits(wifi_event_group);
        if (shutdown_bits & WIFI_SHUTDOWN_INITIATED_BIT) {
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            // Wait for WiFi to actually stop (not shutdown)
            EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_STOPPED_BIT, pdTRUE,
                                                   pdFALSE, pdMS_TO_TICKS(1000));

            if (!(bits & WIFI_STOPPED_BIT)) {
                ESP_LOGW(TAG, "WiFi stop event timeout after 1000ms");
            }
        }
    } else {
        // WiFi started in minimal mode (no netif) - just wait a bit for driver to stop
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // CRITICAL: Wait for WiFi driver to finish processing all packets
    // before destroying netif to prevent NULL pointer dereference
    vTaskDelay(pdMS_TO_TICKS(200));

    if (has_netif) {
        xEventGroupClearBits(wifi_event_group, WIFI_STA_CONNECTED_BIT | WIFI_AP_STARTED_BIT |
                                                   WIFI_SHUTDOWN_INITIATED_BIT |
                                                   WIFI_AP_CLIENT_CONNECTED_BIT);
    }

    if (sta_netif) {
        esp_netif_destroy_default_wifi(sta_netif);
        sta_netif = NULL;
    }

    if (ap_netif) {
        esp_netif_destroy_default_wifi(ap_netif);
        ap_netif = NULL;
    }

    return err;
}

void wifi_shutdown() {
    ESP_LOGI(TAG, "Shutting down WiFi subsystem...");
    xEventGroupSetBits(wifi_event_group, WIFI_SHUTDOWN_INITIATED_BIT);

    wifi_unregister_event_handlers();
    wifi_stop();

    esp_err_t err = esp_wifi_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi deinit failed: %s", esp_err_to_name(err));
    }

    // Deinitialization is not supported yet, esp-netif keeps internal state and RAM.x
    // err = esp_netif_deinit();

    ESP_LOGI(TAG, "WiFi subsystem shutdown complete");
}

static void wifi_configure(const wifi_credentials_t *creds) {

    if (creds) {
        wifi_creds = *creds;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    static StaticEventGroup_t event_group_storage;

    if (wifi_event_group == NULL) {
        wifi_event_group = xEventGroupCreateStatic(&event_group_storage);
    } else {
        xEventGroupClearBits(wifi_event_group, WIFI_SHUTDOWN_INITIATED_BIT);
    }

    if (!wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create WiFi event group!");
    }

    if (sta_reconnect_timer == NULL) {
        const esp_timer_create_args_t timer_args = {.callback = wifi_sta_reconnect,
                                                    .name = "sta_reconnect"};
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &sta_reconnect_timer));
    }

    if (ap_idle_timer == NULL) {
        const esp_timer_create_args_t ap_timer_args = {.callback = wifi_ap_idle_timeout,
                                                       .name = "ap_idle"};
        ESP_ERROR_CHECK(esp_timer_create(&ap_timer_args, &ap_idle_timer));
    }
}

esp_err_t wifi_start(const wifi_credentials_t *creds) {

    wifi_configure(creds);

    if (sta_netif == NULL) {
        sta_netif = esp_netif_create_default_wifi_sta();
    }

    // The AP config can only be set while the AP is part of the mode. Before esp_wifi_start() the
    // mode only takes effect at start, so this stores it without the AP ever going on air - a later
    // wifi_ap_enable() brings the AP up with the right SSID from the first beacon
    if (wifi_creds.ap_ssid && strlen(wifi_creds.ap_ssid) > 0) {
        wifi_config_t ap_config = {.ap = {.max_connection = 1, .authmode = WIFI_AUTH_OPEN}};
        strncpy((char *)ap_config.ap.ssid, wifi_creds.ap_ssid, sizeof(ap_config.ap.ssid) - 1);

        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG_AP, "set_mode(APSTA) failed");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG_AP,
                            "set_config(AP) failed");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG_STA, "set_mode(STA) failed");

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG_STA, "start failed");

    if (wifi_creds.sta_ssid && strlen(wifi_creds.sta_ssid) > 0) {
        return wifi_sta_connect(wifi_creds.sta_ssid, wifi_creds.sta_password);
    }

    // Nothing to connect to: the AP is the only way to set the device up, so it opens right away
    // and stays (like one turned on by hand)
    ESP_LOGW(TAG_AP, "Opening AP - no STA network configured");
    ap_auto_allowed = false;
    wifi_ap_enable();
    return ESP_OK;
}

esp_err_t wifi_sta_connect(const char *ssid, const char *password) {

    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGW(TAG_STA, "STA SSID is NULL or empty, cannot connect");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_creds.sta_ssid = ssid;
    wifi_creds.sta_password = password;

    // The next attempt hands them to the driver (see wifi_sta_reconnect)
    sta_creds_changed = true;

    wifi_sta_reconnect_reset();

    // Drops the current network and aborts an attempt in progress. The STA_DISCONNECTED that
    // follows is handled right away (the event task has the higher priority) and starts attempt 1
    esp_wifi_disconnect();

    // Nothing to disconnect, so no event started the attempts - start them here
    if (sta_reconnect_attempt == 0) {
        wifi_sta_schedule_reconnect();
    }

    return ESP_OK;
}

uint16_t wifi_scan(wifi_ap_record_t *records, uint16_t max) {

    bool connected = xEventGroupGetBits(wifi_event_group) & WIFI_STA_CONNECTED_BIT;

    // No attempts during the scan: the driver refuses to scan while STA connects. A connected STA
    // can scan as it is, otherwise an attempt in progress gets aborted
    sta_scanning = true;

    // Where the attempts were, to resume them after the scan: waiting for the next one (in a burst
    // or in the pause after one) or in the middle of one
    bool attempt_waiting = esp_timer_is_active(sta_reconnect_timer);
    uint64_t attempt_at_us = 0;

    if (attempt_waiting) {
        esp_timer_get_expiry_time(sta_reconnect_timer, &attempt_at_us);
    }
    esp_timer_stop(sta_reconnect_timer);

    if (!connected) {
        esp_wifi_disconnect();
    }

    ESP_LOGI(TAG_STA, "Scanning for networks...");

    uint16_t count = 0;
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) {
        // Records come one by one and are filtered while added: one per network name (mesh nodes
        // and extenders broadcast the same SSID, the strongest one is kept, like phones list them),
        // hidden networks dropped. The loop always drains the list, so the driver frees all of it
        wifi_ap_record_t record;
        while (esp_wifi_scan_get_ap_record(&record) == ESP_OK) {
            if (record.ssid[0] == '\0') {
                continue;
            }

            uint16_t i = 0;
            while (i < count && strcmp((const char *)records[i].ssid, (const char *)record.ssid)) {
                i++;
            }

            if (i < count) {
                if (record.rssi > records[i].rssi) {
                    records[i] = record;
                }
            } else if (count < max) {
                records[count++] = record;
            }
        }

        ESP_LOGI(TAG_STA, "Scan done, %d networks found", count);
        for (uint16_t i = 0; i < count; i++) {
            ESP_LOGI(TAG_STA, "  '%s' rssi %d, channel %d", (const char *)records[i].ssid,
                     records[i].rssi, records[i].primary);
        }
    } else {
        ESP_LOGE(TAG_STA, "Scan failed: %s", esp_err_to_name(err));
    }

    sta_scanning = false;

    // Back to the attempts where they were - the scan doesn't touch the counter or the pause.
    // Waiting: the same wait goes on (right away if it ran out during the scan). An attempt in
    // progress was aborted by the scan: it's repeated right away. No attempts at all (no SSID):
    // nothing to resume
    if (!connected) {
        if (attempt_waiting) {
            int64_t left_us = (int64_t)attempt_at_us - esp_timer_get_time();
            esp_timer_start_once(sta_reconnect_timer, left_us > 0 ? left_us : 1);
        } else if (sta_reconnect_attempt > 0) {
            wifi_sta_reconnect(NULL);
        }
    }

    return count;
}

void wifi_ap_enable(void) {

    if (!wifi_creds.ap_ssid || strlen(wifi_creds.ap_ssid) == 0) {
        ESP_LOGW(TAG_AP, "AP SSID is NULL or empty, cannot enable AP");
        return;
    }

    // The AP interface lives from the first enable on; switching it off only changes the mode
    if (ap_netif == NULL) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    // Enabling an AP that already runs just extends it
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA) {
        EventBits_t bits = xEventGroupGetBits(wifi_event_group);
        if ((bits & WIFI_STA_CONNECTED_BIT) && !(bits & WIFI_AP_CLIENT_CONNECTED_BIT)) {
            wifi_ap_idle_countdown_start(CONFIG_WIFI_AP_INACTIVITY_TIMEOUT_MINUTES * 60,
                                         "extended, STA is connected");
        } else if (ap_unused_since_auto_open) {
            // Turned on by hand over an auto-opened AP: no limit while STA has no IP
            ap_unused_since_auto_open = false;
            esp_timer_stop(ap_idle_timer);
        }
        ESP_LOGI(TAG_AP, "AP already running");
        return;
    }

    // The countdown starts on WIFI_EVENT_AP_START
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_AP, "set_mode(APSTA) failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG_AP, "Access Point enabled next to STA (SSID: %s)", wifi_creds.ap_ssid);
}

void wifi_ap_disable(void) {

    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode != WIFI_MODE_APSTA) {
        ESP_LOGI(TAG_AP, "AP not running");
        return;
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_AP, "set_mode(STA) failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG_AP, "Access Point disabled, STA only");
}

void wifi_get_interface_ip(char *buf, size_t buflen) {

    esp_netif_ip_info_t ip_info;
    esp_netif_t *active = NULL;

    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        active = sta_netif;
    } else if (ap_netif && esp_netif_is_netif_up(ap_netif) &&
               esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        active = ap_netif;
    }
    if (active) {
        snprintf(buf, buflen, "%d.%d.%d.%d", esp_ip4_addr1(&ip_info.ip), esp_ip4_addr2(&ip_info.ip),
                 esp_ip4_addr3(&ip_info.ip), esp_ip4_addr4(&ip_info.ip));
    } else {
        snprintf(buf, buflen, "0.0.0.0");
    }
}

// Both skip the driver while STA isn't connected - esp_wifi_sta_get_ap_info() logs a warning then

int8_t wifi_get_rssi(void) {
    wifi_ap_record_t ap;
    if (!wifi_event_group || !(xEventGroupGetBits(wifi_event_group) & WIFI_STA_CONNECTED_BIT))
        return 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        return ap.rssi;
    return 0;
}

bool wifi_get_ssid(char *buf, size_t buflen) {
    wifi_ap_record_t ap;
    if (!wifi_event_group || !(xEventGroupGetBits(wifi_event_group) & WIFI_STA_CONNECTED_BIT))
        return false;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
        return false;
    snprintf(buf, buflen, "%s", (const char *)ap.ssid);
    return true;
}

void wifi_log_event_group_bits(void) {

    if (!wifi_event_group)
        return;

    EventBits_t bits = xEventGroupGetBits(wifi_event_group);
    ESP_LOGI(TAG, "WiFi bits: %s%s%s", (bits & WIFI_STA_CONNECTED_BIT) ? "STA " : "",
             (bits & WIFI_AP_STARTED_BIT) ? "AP " : "",
             (bits & WIFI_AP_CLIENT_CONNECTED_BIT) ? "AP_CLIENT " : "");
}
