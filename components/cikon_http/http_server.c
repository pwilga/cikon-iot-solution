#include "http_server.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

#define TAG "cikon:http"
#define WWW_ROOT CONFIG_VFS_LITTLEFS_MOUNT_POINT "/www"

static httpd_handle_t s_server = NULL;
static char s_path[sizeof(WWW_ROOT) + CONFIG_HTTPD_MAX_URI_LEN];

static void http_log(int status, const char *path, size_t bytes) {
    if (status >= 500)
        ESP_LOGE(TAG, "[%d] %s", status, path);
    else if (status >= 400)
        ESP_LOGW(TAG, "[%d] %s", status, path);
    else
        ESP_LOGI(TAG, "[%d] %s (%zu B)", status, path, bytes);
}

/* Registered as the 404 error handler instead of a wildcard URI handler.
 * Wildcard would have to be registered last to not shadow other routes — which
 * is impossible to guarantee when JSON endpoints are added dynamically after
 * http_init().  The 404 path fires only after all registered handlers fail to
 * match, so dynamic endpoints always take priority regardless of order.
 * Also doubles as an SPA fallback: "/" → index.html. */
static esp_err_t static_file_handler(httpd_req_t *req, httpd_err_code_t err) {
    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0)
        uri = "/index.html";

    snprintf(s_path, sizeof(s_path), WWW_ROOT "%s", uri);

    FILE *f = fopen(s_path, "r");
    if (!f) {
        http_log(404, s_path, 0);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_FAIL;
    }

    const char *ct = "text/html";
    if (strstr(s_path, ".css"))
        ct = "text/css";
    else if (strstr(s_path, ".js"))
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
    http_log(200, s_path, total);
    return ESP_OK;
}

static esp_err_t json_get_handler(httpd_req_t *req) {

    http_json_get_fn_t fn = req->user_ctx;
    cJSON *root = cJSON_CreateObject();
    if (fn)
        fn(root);
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

static esp_err_t json_post_handler(httpd_req_t *req) {

    http_json_post_fn_t fn = req->user_ctx;
    int len = req->content_len;
    char *buf = malloc(len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_FAIL;
    }
    int received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, buf + received, len - received);
        if (r <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';
    if (fn)
        fn(buf);
    free(buf);
    return httpd_resp_sendstr(req, "OK");
}

void http_register_json_get(const char *uri, http_json_get_fn_t fn) {
    if (!s_server) {
        ESP_LOGE(TAG, "http_init() must be called first");
        return;
    }
    httpd_uri_t ep = {.uri = uri, .method = HTTP_GET, .handler = json_get_handler, .user_ctx = fn};
    httpd_register_uri_handler(s_server, &ep);
}

void http_register_json_post(const char *uri, http_json_post_fn_t fn) {
    if (!s_server) {
        ESP_LOGE(TAG, "http_init() must be called first");
        return;
    }
    httpd_uri_t ep = {
        .uri = uri, .method = HTTP_POST, .handler = json_post_handler, .user_ctx = fn};
    httpd_register_uri_handler(s_server, &ep);
}

static esp_err_t upload_handler(httpd_req_t *req) {
    char query[128] = {};
    char filename[64] = "index.html";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "f", filename, sizeof(filename));
    }

    snprintf(s_path, sizeof(s_path), WWW_ROOT "/%s", filename);

    FILE *f = fopen(s_path, "w");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open file");
        return ESP_FAIL;
    }

    char buf[512];
    int remaining = req->content_len;
    while (remaining > 0) {
        int got =
            httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (got <= 0) {
            fclose(f);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        fwrite(buf, 1, got, f);
        remaining -= got;
    }
    fclose(f);

    http_log(200, s_path, req->content_len);
    httpd_resp_sendstr(req, "OK\n");
    return ESP_OK;
}

static const httpd_uri_t s_upload_uri = {
    .uri = "/upload",
    .method = HTTP_POST,
    .handler = upload_handler,
};

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
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start");
        return;
    }

    httpd_register_uri_handler(s_server, &s_upload_uri);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, static_file_handler);

    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
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
