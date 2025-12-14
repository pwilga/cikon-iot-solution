#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "lwip/sockets.h"
#include "mbedtls/md5.h"

#include "platform_services.h"
#include <stdint.h>

#define RETURN_IF_FALSE(x)                                                                         \
    do {                                                                                           \
        if (!(x))                                                                                  \
            return;                                                                                \
    } while (0)

#define RX_BUFFER_SIZE 1024
#define MAGIC_BYTES 0xAF, 0xCA, 0xEC, 0x2D, 0xFE, 0x55
#define ACK_CODE 0xAA, 0x55

#define MD5_SIZE 16
#define SW_VERSION_SIZE 2

#define TAG "cikon:ota"

static TaskHandle_t ota_task_handle = NULL;

static volatile bool ota_shutdown_requested = false;
static volatile bool ota_update_in_progress = false;

static int ota_listen_sock = -1;
static uint16_t ota_listen_port = CONFIG_TCP_OTA_PORT;

static const char *ota_steps[] = {
    "Waiting for OTA magic header", "Receiving firmware metadata (version, size, MD5)",
    "Initializing OTA update",      "Writing firmware",
    "Verifying firmware",           "Rebooting"};

static const size_t ota_steps_count = sizeof(ota_steps) / sizeof(ota_steps[0]);

#define OTA_LOG_STEP(n)                                                                            \
    if ((n) < ota_steps_count)                                                                     \
    ESP_LOGI(TAG, "[%d/%d] %s...", (n) + 1, ota_steps_count, ota_steps[n])

void tcp_ota_configure(uint16_t port) { ota_listen_port = port; }

static bool send_ack(const int client_sock) {
    const uint8_t ack_code[] = {ACK_CODE};

    bool result = send(client_sock, ack_code, sizeof(ack_code), 0) == sizeof(ack_code);

    if (!result)
        ESP_LOGE(TAG, "Failed to send ACK: errno %d", errno);
    // else
    //     ESP_LOGI(TAG, "ACK sent successfully");

    return result;
}

int read_all(const int client_sock, uint8_t *rx_buffer) {
    int len = recv(client_sock, rx_buffer, RX_BUFFER_SIZE, 0);

    if (len < 0)
        ESP_LOGE(TAG, "Receiving data failed: errno %d", errno);
    else if (!len)
        ESP_LOGW(TAG, "Connection closed");

    return len <= 0 ? 0 : len;
}

static void handle_ota(const int client_sock) {
    uint32_t firmware_size = 0;
    uint8_t sw_version[SW_VERSION_SIZE];
    uint8_t md5_expected[MD5_SIZE];

    uint8_t rx_buffer[RX_BUFFER_SIZE];
    const uint8_t magic_bytes[] = {MAGIC_BYTES};

    // Mark that update is in progress (cannot be interrupted)
    ota_update_in_progress = true;

    OTA_LOG_STEP(0); // Magic header
    if (read_all(client_sock, rx_buffer) == sizeof(magic_bytes)) {
        if (memcmp(rx_buffer, magic_bytes, sizeof(magic_bytes))) {
            ESP_LOGE(TAG, "Invalid OTA magic header");
            ota_update_in_progress = false;
            return;
        }
        RETURN_IF_FALSE(send_ack(client_sock));
    }

    OTA_LOG_STEP(1); // Version, Size, MD5
    if (read_all(client_sock, rx_buffer) == sizeof(sw_version)) {
        memcpy(sw_version, rx_buffer, sizeof(sw_version));
        RETURN_IF_FALSE(send_ack(client_sock));
    }

    if (read_all(client_sock, rx_buffer) == sizeof(uint32_t)) {
        firmware_size = ((uint32_t)rx_buffer[0] << 24) | ((uint32_t)rx_buffer[1] << 16) |
                        ((uint32_t)rx_buffer[2] << 8) | ((uint32_t)rx_buffer[3]);

        RETURN_IF_FALSE(send_ack(client_sock));
    }

    if (read_all(client_sock, rx_buffer) == sizeof(md5_expected)) {
        memcpy(md5_expected, rx_buffer, sizeof(md5_expected));
        RETURN_IF_FALSE(send_ack(client_sock));
    }

    OTA_LOG_STEP(2); // OTA start

    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    esp_err_t err = esp_ota_begin(update_partition, firmware_size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to begin OTA update: %s", esp_err_to_name(err));
        return;
    }

    // Initialize MD5 context
    mbedtls_md5_context md5_ctx;
    mbedtls_md5_init(&md5_ctx);
    mbedtls_md5_starts(&md5_ctx);

    // Ready for image transmission

    OTA_LOG_STEP(3); // Write

    uint32_t total_read_bytes = 0;
    uint16_t read_bytes = -1;
    uint8_t last_percent_reported = -1;

    while (read_bytes) {
        read_bytes = read_all(client_sock, rx_buffer);
        total_read_bytes += read_bytes;

        int percent = (100 * total_read_bytes) / firmware_size;
        if (percent / 10 != last_percent_reported / 10) {
            ESP_LOGI(TAG, "Progress: %d%% (%d/%d bytes)", percent, total_read_bytes, firmware_size);
            last_percent_reported = percent;
        }

        err = esp_ota_write(ota_handle, rx_buffer, read_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write OTA data chunk (%d bytes): %s", read_bytes,
                     esp_err_to_name(err));
            esp_ota_end(ota_handle);
            return;
        }
        mbedtls_md5_update(&md5_ctx, rx_buffer, read_bytes);
    }

    RETURN_IF_FALSE(send_ack(client_sock));

    OTA_LOG_STEP(4); // Verification
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Finalizing OTA update failed: %s", esp_err_to_name(err));
        return;
    }

    // Finalize MD5 calculation
    uint8_t md5_calc[MD5_SIZE];
    mbedtls_md5_finish(&md5_ctx, md5_calc);
    mbedtls_md5_free(&md5_ctx);

    // Compare calculated MD5 with expected
    if (memcmp(md5_calc, md5_expected, sizeof(md5_expected))) {
        ESP_LOGE(TAG, "MD5 mismatch!");
        return;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Setting new boot partition failed: %s", esp_err_to_name(err));
        ota_update_in_progress = false;
        return;
    }

    ota_update_in_progress = false;

    OTA_LOG_STEP(5); // Reboot
    esp_safe_restart();
}

void tcp_ota_task(void *args) {

    struct sockaddr_storage dest_addr;

    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(ota_listen_port);

    ota_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (ota_listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket. System error code: %d", errno);
        ota_listen_sock = -1;
        ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    /*
        SO_REUSEADDR option allows to reuse the same IP address and port even
        if they were recently used and are still in the TIME_WAIT state.
    */
    int opt = 1;
    setsockopt(ota_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(ota_listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) ||
        listen(ota_listen_sock, 1)) {
        ESP_LOGE(TAG, "Socket setup failed. Step: %s, Error code: %d",
                 (errno == EADDRINUSE || errno == EADDRNOTAVAIL) ? "bind" : "listen", errno);
        close(ota_listen_sock);
        ota_listen_sock = -1;
        ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (!ota_shutdown_requested) {
        ESP_LOGI(TAG, "Listening for connections, port: %d.", ota_listen_port);

        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int client_sock = accept(ota_listen_sock, (struct sockaddr *)&source_addr, &addr_len);

        if (client_sock < 0) {
            // Socket was closed from shutdown, this is expected
            if (ota_shutdown_requested) {
                ESP_LOGI(TAG, "OTA shutdown requested");
            } else {
                ESP_LOGE(TAG, "Error accepting incoming connection: errno %d", errno);
            }
            break;
        }

        char addr_str[16];
        inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(TAG, "Incoming connection from %s", addr_str);

        handle_ota(client_sock);

        shutdown(client_sock, SHUT_RDWR);
        close(client_sock);
    }

    if (ota_listen_sock >= 0) {
        close(ota_listen_sock);
        ota_listen_sock = -1;
    }

    ota_shutdown_requested = false;
    ota_task_handle = NULL;
    ESP_LOGE(TAG, "OTA task exiting");
    vTaskDelete(NULL);
}

void tcp_ota_init(void) {
    if (ota_task_handle != NULL) {
        ESP_LOGW(TAG, "OTA task already running");
        return;
    }

    ota_shutdown_requested = false;
    xTaskCreate(tcp_ota_task, "tcp_ota", CONFIG_TCP_OTA_TASK_STACK_SIZE, NULL,
                CONFIG_TCP_OTA_TASK_PRIORITY, &ota_task_handle);
}

void tcp_ota_shutdown(void) {
    if (ota_task_handle == NULL) {
        ESP_LOGW(TAG, "OTA task not running");
        return;
    }

    // ESP_LOGI(TAG, "Shutting down OTA service");

    // Signal task to stop
    ota_shutdown_requested = true;

    // Close listen socket to unblock accept()
    if (ota_listen_sock >= 0) {
        shutdown(ota_listen_sock, SHUT_RDWR);
        close(ota_listen_sock);
        ota_listen_sock = -1;
    }

    // Wait for task to finish cleanup and set handle to NULL
    // If update is in progress, this will wait until it completes
    uint8_t wait_count = 0;
    bool logged_waiting = false;

    while (ota_task_handle != NULL && (ota_update_in_progress || wait_count < 100)) {

        if (ota_update_in_progress && !logged_waiting) {
            ESP_LOGW(TAG, "Waiting for OTA update to complete before shutdown...");
            logged_waiting = true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_count++;
    }
}
