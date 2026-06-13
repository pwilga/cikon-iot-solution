#include "cikon_http.h"
#include "esp_log.h"

#define TAG "cikon:http"

static httpd_handle_t s_server = NULL;
static const httpd_uri_t *s_queue[CONFIG_HTTP_MAX_HANDLERS];
static int s_queue_len = 0;

static esp_err_t hello_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"hello\":\"cikon_http\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t s_hello_uri = {
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = hello_get_handler,
};

esp_err_t http_register_uri(const httpd_uri_t *uri)
{
    if (s_server) {
        return httpd_register_uri_handler(s_server, uri);
    }
    if (s_queue_len >= CONFIG_HTTP_MAX_HANDLERS) {
        ESP_LOGE(TAG, "handler queue full");
        return ESP_ERR_NO_MEM;
    }
    s_queue[s_queue_len++] = uri;
    return ESP_OK;
}

void http_init(void)
{
    if (s_server) {
        return;
    }
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.server_port      = CONFIG_HTTP_PORT;
    config.stack_size       = CONFIG_HTTP_STACK_SIZE;
    config.max_uri_handlers = CONFIG_HTTP_MAX_HANDLERS;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start");
        return;
    }

    httpd_register_uri_handler(s_server, &s_hello_uri);
    for (int i = 0; i < s_queue_len; i++) {
        httpd_register_uri_handler(s_server, s_queue[i]);
    }
    ESP_LOGI(TAG, "started on port %d", CONFIG_HTTP_PORT);
}

void http_shutdown(void)
{
    if (!s_server) {
        return;
    }
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "stopped");
}
