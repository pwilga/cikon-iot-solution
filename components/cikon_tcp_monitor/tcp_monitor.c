#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/ringbuf.h"
#include "freertos/task.h"

#include "sdkconfig.h"
#include "task_helpers.h"
#include "tcp_monitor.h"

#define TAG "cikon:monitor"

static uint16_t tcp_monitor_port = 6666;
static int monitor_sock = -1;
static int client_sock = -1;
static RingbufHandle_t ring_buffer = NULL;
static volatile TaskHandle_t monitor_task_handle = NULL;
static volatile bool shutdown_requested = false;

static vprintf_like_t original_vprintf = NULL;

// Called on every ESP_LOG*() even without clients. Ring buffer overhead is minimal.
static int tcp_monitor_vprintf(const char *fmt, va_list args) {
    // Forward to original UART vprintf
    if (original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    // Format for ring buffer
    char buffer[CONFIG_MAX_LOG_SIZE];
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (len > 0 && ring_buffer != NULL) {
        if (len >= sizeof(buffer))
            len = sizeof(buffer) - 1;
        xRingbufferSend(ring_buffer, buffer, len, 0);
    }

    return len;
}

static void tcp_monitor_task(void *args) {

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(tcp_monitor_port)};

    monitor_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (monitor_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(monitor_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(monitor_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0 ||
        listen(monitor_sock, 1) < 0) {
        ESP_LOGE(TAG, "Failed to bind/listen");
        close(monitor_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP Monitor on port %d", tcp_monitor_port);

    while (!shutdown_requested) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        client_sock = accept(monitor_sock, (struct sockaddr *)&client_addr, &len);

        if (client_sock < 0) {
            if (shutdown_requested) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "Client connected: %s:%d", inet_ntoa(client_addr.sin_addr),
                 ntohs(client_addr.sin_port));

        while (!shutdown_requested) {
            size_t size;
            char *data = (char *)xRingbufferReceive(ring_buffer, &size, pdMS_TO_TICKS(100));

            if (data != NULL) {
                // Detect log level by first character for fast lookup
                const char *color = "\033[0m"; // Default: white
                if (data[1] == ' ' && data[2] == '(') {
                    switch (data[0]) {
                    case 'I':
                        color = "\033[0;32m";
                        break; // Green
                    case 'W':
                        color = "\033[0;33m";
                        break; // Yellow
                    case 'E':
                        color = "\033[0;31m";
                        break; // Red
                    }
                }

                // Format with color codes in single buffer
                char output[CONFIG_MAX_LOG_SIZE + 16]; // +16 for ANSI codes (max 11 bytes)
                int len = snprintf(output, sizeof(output), "%s%.*s\033[0m", color, (int)size, data);

                vRingbufferReturnItem(ring_buffer, data);

                if (send(client_sock, output, len, 0) <= 0) {
                    break; // Disconnected
                }
            }
        }

        ESP_LOGW(TAG, "Client disconnected: %s", inet_ntoa(client_addr.sin_addr));
        if (client_sock >= 0) {
            shutdown(client_sock, SHUT_RDWR);
            close(client_sock);
            client_sock = -1;
        }
    }

    if (monitor_sock >= 0) {
        close(monitor_sock);
        monitor_sock = -1;
    }

    ESP_LOGI(TAG, "TCP monitor task exiting");
    shutdown_requested = false;
    monitor_task_handle = NULL;
    vTaskDelete(NULL);
}

void tcp_monitor_configure(uint16_t port) { tcp_monitor_port = port; }

void tcp_monitor_init(void) {

    if (monitor_task_handle != NULL) {
        ESP_LOGW(TAG, "TCP Monitor already running");
        return;
    }

    ring_buffer = xRingbufferCreate(CONFIG_RING_BUFFER_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (ring_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return;
    }

    original_vprintf = esp_log_set_vprintf(tcp_monitor_vprintf);
    xTaskCreate(tcp_monitor_task, "tcp_monitor", CONFIG_TCP_MONITOR_TASK_STACK_SIZE, NULL,
                CONFIG_TCP_MONITOR_TASK_PRIORITY, (TaskHandle_t *)&monitor_task_handle);
}

void tcp_monitor_shutdown(void) {
    if (monitor_task_handle == NULL) {
        return;
    }

    if (original_vprintf) {
        esp_log_set_vprintf(original_vprintf);
        original_vprintf = NULL;
    }

    shutdown_requested = true;

    // Close listen socket to unblock accept()
    if (monitor_sock >= 0) {
        shutdown(monitor_sock, SHUT_RDWR);
        close(monitor_sock);
        monitor_sock = -1;
    }

    if (!task_wait_for_finish(&monitor_task_handle, 2000)) {
        ESP_LOGW(TAG, "TCP Monitor task did not finish in time");
    }

    if (ring_buffer != NULL) {
        vRingbufferDelete(ring_buffer);
        ring_buffer = NULL;
    }
}