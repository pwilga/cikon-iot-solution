#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "cJSON.h"
#include <esp_log.h>
#include <esp_system.h>

#include "certs.h"
#include "https_server.h"
#include "task_helpers.h"

#define TAG "cikon:https"

#define HTTPS_SHUTDOWN_INITIATED_BIT BIT0
#define HTTPS_SERVER_STARTED_BIT BIT1

static TimerHandle_t https_inactivity_timer = NULL;
static EventGroupHandle_t https_event_group = NULL;
static httpd_handle_t https_server_handle = NULL;
static volatile TaskHandle_t https_server_task_handle = NULL;

static const char *http_auth_string = NULL;
static const https_endpoint_config_t *registered_endpoints = NULL;

void https_configure(const https_endpoint_config_t *endpoints, const char *http_auth) {
    registered_endpoints = endpoints;
    http_auth_string = http_auth;
}

static void https_inactivity_timer_callback(TimerHandle_t xTimer) {
    ESP_LOGW(TAG, "HTTPS inactivity timer: %.1f s timeout, restarting server",
             CONFIG_HTTPS_INACTIVITY_TIMEOUT_MS / 1000.0f);
    https_shutdown();
    https_init();
}

/**
 * @brief Restarts the inactivity timer on every valid HTTP request.
 *
 * This timer replaces the need for HTTP keep-alive: the server is configured for one connection at
 * a time and does not use keep-alive (persistent connections). Each request resets the timer, so
 * the server stays up as long as there is activity, regardless of connection reuse. This approach
 * avoids RAM leaks and blocking issues that can occur with keep-alive on resource-constrained
 * devices like ESP32. When no requests are received for HTTPS_INACTIVITY_TIMEOUT_MS, the server is
 * safely shut down and restarted, ensuring robust operation.
 */
static void https_restart_inactivity_timer(void) {
    if (https_inactivity_timer) {
        xTimerStop(https_inactivity_timer, 0);
        xTimerChangePeriod(https_inactivity_timer,
                           pdMS_TO_TICKS(CONFIG_HTTPS_INACTIVITY_TIMEOUT_MS), 0);
        xTimerStart(https_inactivity_timer, 0);
    }
}

void https_init(void) {

    if (https_server_handle != NULL) {
        ESP_LOGW(TAG, "HTTPS server already running, ignoring init");
        return;
    }

    static StaticEventGroup_t http_event_group_storage;

    if (https_event_group == NULL) {
        https_event_group = xEventGroupCreateStatic(&http_event_group_storage);
    }

    if (!https_event_group) {
        ESP_LOGE(TAG, "Failed to create https event group!");
        return;
    }

    if (!https_inactivity_timer) {
        // Set timer period to 1 tick (minimum, will be changed on use)
        https_inactivity_timer =
            xTimerCreate("https_inact", 1, pdFALSE, NULL, https_inactivity_timer_callback);
    }
    xTaskCreate(https_server_task, "https_serv", CONFIG_HTTPS_TASK_STACK_SIZE, NULL,
                CONFIG_HTTPS_TASK_PRIORITY, (TaskHandle_t *)&https_server_task_handle);
}

void https_shutdown(void) {

    if (https_server_handle == NULL)
        return;

    xEventGroupSetBits(https_event_group, HTTPS_SHUTDOWN_INITIATED_BIT);

    if (!task_wait_for_finish(&https_server_task_handle, 1000)) {
        ESP_LOGE(TAG, "HTTPS server error shutdown timeout");
    }
}

static bool https_check_basic_auth(httpd_req_t *req) {

    if (http_auth_string == NULL || strlen(http_auth_string) == 0) {
        return true;
    }

    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (auth_len == 0) {
        goto unauthorized;
    }
    char *auth_buf = malloc(auth_len + 1);
    if (!auth_buf) {
        httpd_resp_send_500(req);
        return false;
    }
    bool auth_ok =
        (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf, auth_len + 1) == ESP_OK) &&
        (strcmp(auth_buf, http_auth_string) == 0);
    free(auth_buf);
    if (auth_ok) {
        return true;
    }
unauthorized:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
}

static esp_err_t https_get_handler(httpd_req_t *req) {
    if (!https_check_basic_auth(req)) {
        ESP_LOGW(TAG, "GET /tele: unauthorized");
        return ESP_FAIL;
    }

    https_restart_inactivity_timer();

    cJSON *json = cJSON_CreateObject();

    https_json_tele_t json_tele = (https_json_tele_t)req->user_ctx;
    if (json_tele) {
        json_tele(json);
    }

    char *json_str = cJSON_PrintUnformatted(json);

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    free(json_str);
    cJSON_Delete(json);

    return ret;
}

static esp_err_t https_post_handler(httpd_req_t *req) {

    if (!https_check_basic_auth(req)) {
        ESP_LOGW(TAG, "POST /cmnd: unauthorized");
        return ESP_FAIL;
    }

    https_restart_inactivity_timer();

    int total_len = req->content_len;
    char *buf = malloc(total_len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "POST /cmnd: malloc failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            ESP_LOGE(TAG, "POST /cmnd: recv failed, ret=%d", ret);
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[received] = '\0';

    https_json_cmnd_t json_cmnd = (https_json_cmnd_t)req->user_ctx;
    if (json_cmnd) {
        json_cmnd(buf);
    }
    free(buf);

    esp_err_t ret = httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ret;
}

void https_server_start(void) {
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();

    conf.servercert = (const uint8_t *)get_client_pem_start();
    conf.servercert_len = get_client_pem_size();
    conf.prvtkey_pem = (const uint8_t *)get_client_key_start();
    conf.prvtkey_len = get_client_key_size();

    // Limit HTTPS server to only one connection at a time
    httpd_config_t httpd_conf = HTTPD_DEFAULT_CONFIG();
    httpd_conf.max_open_sockets = CONFIG_HTTPS_MAX_OPEN_SOCKETS;
    httpd_conf.lru_purge_enable = true; // Enable LRU purge to close old connections
    httpd_conf.close_fn = NULL;         // Use default close with SO_LINGER
    // httpd_conf.keep_alive_enable = true;
    // httpd_conf.keep_alive_idle = 60; // seconds
    // httpd_conf.keep_alive_interval = 5;
    // httpd_conf.keep_alive_count = 3;
    conf.httpd = httpd_conf;

    esp_err_t ret = httpd_ssl_start(&https_server_handle, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error starting HTTPS server (%d: %s)", ret, esp_err_to_name(ret));
        https_server_handle = NULL;
        return;
    }

    if (registered_endpoints) {
        for (size_t i = 0; registered_endpoints[i].uri != NULL; i++) {
            httpd_uri_t uri = {.uri = registered_endpoints[i].uri,
                               .method = registered_endpoints[i].method,
                               .handler = (registered_endpoints[i].method == HTTP_POST)
                                              ? https_post_handler
                                              : https_get_handler,
                               .user_ctx = (registered_endpoints[i].method == HTTP_POST)
                                               ? (void *)registered_endpoints[i].json_cmnd
                                               : (void *)registered_endpoints[i].json_tele};
            httpd_register_uri_handler(https_server_handle, &uri);
        }
    }
}

void https_server_stop(void) {
    if (https_server_handle) {
        httpd_ssl_stop(https_server_handle);
        https_server_handle = NULL;
    }
    if (https_inactivity_timer) {
        // Delete the inactivity timer to free resources, since the server is stopped
        xTimerDelete(https_inactivity_timer, 0);
        https_inactivity_timer = NULL;
    }
}

void https_server_task(void *args) {

    https_server_start();
    xEventGroupSetBits(https_event_group, HTTPS_SERVER_STARTED_BIT);

    while (!(xEventGroupGetBits(https_event_group) & HTTPS_SHUTDOWN_INITIATED_BIT)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    https_server_stop();
    // ESP_LOGE(TAG, "HTTPS server task exiting");

    xEventGroupClearBits(https_event_group,
                         HTTPS_SHUTDOWN_INITIATED_BIT | HTTPS_SERVER_STARTED_BIT);
    https_server_handle = NULL;
    https_server_task_handle = NULL;
    vTaskDelete(NULL);
}
