#include "http_server.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "tele.h"
#include <stdio.h>
#include <string.h>

#define TAG "cikon:http"
#define WWW_ROOT CONFIG_VFS_LITTLEFS_MOUNT_POINT "/www"

static httpd_handle_t s_server = NULL;
static const httpd_uri_t *s_http_handlers[CONFIG_HTTP_MAX_HANDLERS];
static int s_http_handlers_len = 0;

static void http_log(int status, const char *path, size_t bytes) {
    if (status >= 500)
        ESP_LOGE(TAG, "[%d] %s", status, path);
    else if (status >= 400)
        ESP_LOGW(TAG, "[%d] %s", status, path);
    else
        ESP_LOGI(TAG, "[%d] %s (%zu B)", status, path, bytes);
}

static esp_err_t static_file_handler(httpd_req_t *req) {

    char path[256];
    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0)
        uri = "/index.html";

    snprintf(path, sizeof(path), WWW_ROOT "%s", uri);

    FILE *f = fopen(path, "r");
    if (!f) {
        http_log(404, path, 0);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_OK;
    }

    const char *ct = "text/html";
    if (strstr(path, ".css"))
        ct = "text/css";
    else if (strstr(path, ".js"))
        ct = "application/javascript";

    httpd_resp_set_type(req, ct);

    char buf[1024];
    size_t n;
    size_t total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, n);
        total += n;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    http_log(200, path, total);
    return ESP_OK;
}

/* ── /info ── */

static const char *chip_name(esp_chip_model_t m) {
    switch (m) {
    case CHIP_ESP32:
        return "ESP32";
    case CHIP_ESP32S2:
        return "ESP32-S2";
    case CHIP_ESP32S3:
        return "ESP32-S3";
    case CHIP_ESP32C3:
        return "ESP32-C3";
    case CHIP_ESP32C6:
        return "ESP32-C6";
    case CHIP_ESP32H2:
        return "ESP32-H2";
    default:
        return "ESP32";
    }
}

static esp_err_t info_handler(httpd_req_t *req) {
    const esp_app_desc_t *desc = esp_app_get_description();
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "app", desc->project_name);
    cJSON_AddStringToObject(root, "version", desc->version);
    cJSON_AddStringToObject(root, "idf", desc->idf_ver);
    cJSON_AddStringToObject(root, "chip", chip_name(chip.model));
    cJSON_AddNumberToObject(root, "chip_rev", chip.revision);
    cJSON_AddNumberToObject(root, "cores", chip.cores);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000LL);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, body);
    free(body);
    return ret;
}

/* ── /tele ── */

static esp_err_t tele_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    tele_append_all(root);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, body);
    free(body);
    return ret;
}

/* ── /upload ── */

static esp_err_t upload_handler(httpd_req_t *req) {
    char query[128] = {};
    char filename[64] = "index.html";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "f", filename, sizeof(filename));
    }

    char path[320];
    snprintf(path, sizeof(path), WWW_ROOT "/%s", filename);

    FILE *f = fopen(path, "w");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open file");
        return ESP_FAIL;
    }

    char buf[512];
    int remaining = req->content_len;
    while (remaining > 0) {
        int got = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (got <= 0) {
            fclose(f);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        fwrite(buf, 1, got, f);
        remaining -= got;
    }
    fclose(f);

    http_log(200, path, req->content_len);
    httpd_resp_sendstr(req, "OK\n");
    return ESP_OK;
}

/* ── URI table ── */

static const httpd_uri_t s_static_uri = {
    .uri = "/*",
    .method = HTTP_GET,
    .handler = static_file_handler,
};

static const httpd_uri_t s_info_uri = {
    .uri = "/info",
    .method = HTTP_GET,
    .handler = info_handler,
};

static const httpd_uri_t s_tele_uri = {
    .uri = "/tele",
    .method = HTTP_GET,
    .handler = tele_handler,
};

static const httpd_uri_t s_upload_uri = {
    .uri = "/upload",
    .method = HTTP_POST,
    .handler = upload_handler,
};

/* ── Public API ── */

esp_err_t http_register_uri(const httpd_uri_t *uri) {
    if (s_server) {
        return httpd_register_uri_handler(s_server, uri);
    }
    if (s_http_handlers_len >= CONFIG_HTTP_MAX_HANDLERS) {
        ESP_LOGE(TAG, "handler queue full");
        return ESP_ERR_NO_MEM;
    }
    s_http_handlers[s_http_handlers_len++] = uri;
    return ESP_OK;
}

void http_init(void) {
    if (s_server) {
        ESP_LOGW(TAG, "Already started");
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_HTTP_PORT;
    config.stack_size = CONFIG_HTTP_STACK_SIZE;
    config.max_uri_handlers = CONFIG_HTTP_MAX_HANDLERS;
    config.lru_purge_enable = true;
    config.ctrl_port = CONFIG_HTTP_CTRL_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start");
        return;
    }

    httpd_register_uri_handler(s_server, &s_info_uri);
    httpd_register_uri_handler(s_server, &s_tele_uri);
    httpd_register_uri_handler(s_server, &s_upload_uri);

    for (int i = 0; i < s_http_handlers_len; i++) {
        httpd_register_uri_handler(s_server, s_http_handlers[i]);
    }
    httpd_register_uri_handler(s_server, &s_static_uri);

    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    ESP_LOGI(TAG, "started on port %d", CONFIG_HTTP_PORT);
}

void http_shutdown(void) {
    if (!s_server) {
        ESP_LOGW(TAG, "Already stopped");
        return;
    }
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "stopped");
}
