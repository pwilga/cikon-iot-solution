#include "http_server.h"
#include "cJSON.h"
#include "certs.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#if CONFIG_HTTP_ENABLE_WEBDAV
#include "webdav.h"
#endif

#define TAG "cikon:http"
#define WWW_ROOT CONFIG_VFS_LITTLEFS_MOUNT_POINT "/www"

static httpd_handle_t s_server = NULL;
static bool s_secure = false;
static char s_path[sizeof(WWW_ROOT) + CONFIG_HTTPD_MAX_URI_LEN];

static void http_log(const char *method, int status, const char *path, size_t bytes) {
    if (status >= 500)
        ESP_LOGE(TAG, "%s %s %d", method, path, status);
    else if (status >= 400)
        ESP_LOGW(TAG, "%s %s %d", method, path, status);
    else
        ESP_LOGI(TAG, "%s %s %d %zu B", method, path, status, bytes);
}

static void set_keepalive_timeout(httpd_req_t *req) {
    static char hdr[32];
    snprintf(hdr, sizeof(hdr), "timeout=%d", CONFIG_HTTP_SESSION_TIMEOUT);
    httpd_resp_set_hdr(req, "Keep-Alive", hdr);
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
        http_log("GET", 404, s_path, 0);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_FAIL;
    }

    const char *ct = "text/html";
    if (strstr(s_path, ".css"))
        ct = "text/css";
    else if (strstr(s_path, ".js"))
        ct = "application/javascript";

    set_keepalive_timeout(req);
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
    http_log("GET", 200, s_path, total);
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
        http_log("GET", 500, req->uri, 0);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_FAIL;
    }
    set_keepalive_timeout(req);
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
        http_log("POST", 500, req->uri, 0);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_FAIL;
    }
    int received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, buf + received, len - received);
        if (r <= 0) {
            http_log("POST", 500, req->uri, 0);
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
    http_log("POST", 200, req->uri, len);
    set_keepalive_timeout(req);
    return httpd_resp_sendstr(req, "OK");
}

static void register_common_handlers(void) {
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, static_file_handler);
#if CONFIG_HTTP_ENABLE_WEBDAV
    http_webdav_init(s_server);
#endif
}

void http_init(const http_config_t *cfg) {
    if (s_server) {
        ESP_LOGW(TAG, "Already started");
        return;
    }
    s_secure = cfg->secure;

    if (cfg->secure) {
        if (!certs_available()) {
            ESP_LOGE(TAG, "Certificates not available");
            return;
        }
        httpd_ssl_config_t ssl_cfg = HTTPD_SSL_CONFIG_DEFAULT();
        ssl_cfg.servercert = (const uint8_t *)get_client_pem_start();
        ssl_cfg.servercert_len = get_client_pem_size();
        ssl_cfg.prvtkey_pem = (const uint8_t *)get_client_key_start();
        ssl_cfg.prvtkey_len = get_client_key_size();
        ssl_cfg.httpd.stack_size = CONFIG_HTTPS_STACK_SIZE;
        ssl_cfg.httpd.recv_wait_timeout = CONFIG_HTTP_SESSION_TIMEOUT;
        ssl_cfg.httpd.send_wait_timeout = CONFIG_HTTP_SESSION_TIMEOUT;
        ssl_cfg.httpd.max_uri_handlers = CONFIG_HTTP_MAX_HANDLERS;
        ssl_cfg.httpd.max_open_sockets = cfg->max_open_sockets ? cfg->max_open_sockets : 1;
        ssl_cfg.httpd.lru_purge_enable = true;
        ssl_cfg.httpd.ctrl_port = cfg->ctrl_port;
        ssl_cfg.httpd.uri_match_fn = httpd_uri_match_wildcard;
        if (httpd_ssl_start(&s_server, &ssl_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start HTTPS");
            s_server = NULL;
            return;
        }
    } else {
        httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
        http_cfg.server_port = cfg->port;
        http_cfg.stack_size = CONFIG_HTTP_STACK_SIZE;
        http_cfg.max_uri_handlers = CONFIG_HTTP_MAX_HANDLERS;
        http_cfg.recv_wait_timeout = CONFIG_HTTP_SESSION_TIMEOUT;
        http_cfg.send_wait_timeout = CONFIG_HTTP_SESSION_TIMEOUT;
        http_cfg.lru_purge_enable = true;
        http_cfg.ctrl_port = cfg->ctrl_port;
        http_cfg.uri_match_fn = httpd_uri_match_wildcard;
        if (httpd_start(&s_server, &http_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start HTTP");
            s_server = NULL;
            return;
        }
    }

    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    ESP_LOGI(TAG, "Started on port %d (%s)", cfg->port, cfg->secure ? "https" : "http");
    register_common_handlers();
}

void http_shutdown(void) {
    if (!s_server) {
        ESP_LOGW(TAG, "Already stopped");
        return;
    }
    if (s_secure)
        httpd_ssl_stop(s_server);
    else
        httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "Stopped");
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
