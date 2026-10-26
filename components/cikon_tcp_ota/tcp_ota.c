#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "lwip/sockets.h"
#include "psa/crypto.h"

#include "platform_services.h"
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

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

    esp_err_t ret = ESP_OK;
    esp_ota_handle_t ota_handle = 0;
    psa_hash_operation_t hash_op = PSA_HASH_OPERATION_INIT;
    const esp_partition_t *update_partition = NULL;

    // Mark that update is in progress (cannot be interrupted)
    ota_update_in_progress = true;

    // Every header part must arrive whole - a client that isn't an OTA uploader (port scan,
    // browser) would otherwise leave firmware_size at 0 and the progress calculation divides by it
    OTA_LOG_STEP(0); // Magic header
    ESP_GOTO_ON_FALSE(read_all(client_sock, rx_buffer) == sizeof(magic_bytes), ESP_ERR_INVALID_SIZE,
                      cleanup, TAG, "No OTA magic header");
    ESP_GOTO_ON_FALSE(!memcmp(rx_buffer, magic_bytes, sizeof(magic_bytes)), ESP_ERR_INVALID_ARG,
                      cleanup, TAG, "Invalid OTA magic header");
    ESP_GOTO_ON_FALSE(send_ack(client_sock), ESP_FAIL, cleanup, TAG, "ACK failed");
    vTaskDelay(1);

    OTA_LOG_STEP(1); // Version, Size, MD5
    ESP_GOTO_ON_FALSE(read_all(client_sock, rx_buffer) == sizeof(sw_version), ESP_ERR_INVALID_SIZE,
                      cleanup, TAG, "No firmware version");
    memcpy(sw_version, rx_buffer, sizeof(sw_version));
    ESP_GOTO_ON_FALSE(send_ack(client_sock), ESP_FAIL, cleanup, TAG, "ACK failed");
    vTaskDelay(1);

    ESP_GOTO_ON_FALSE(read_all(client_sock, rx_buffer) == sizeof(uint32_t), ESP_ERR_INVALID_SIZE,
                      cleanup, TAG, "No firmware size");
    firmware_size = ((uint32_t)rx_buffer[0] << 24) | ((uint32_t)rx_buffer[1] << 16) |
                    ((uint32_t)rx_buffer[2] << 8) | ((uint32_t)rx_buffer[3]);
    ESP_GOTO_ON_FALSE(firmware_size > 0, ESP_ERR_INVALID_SIZE, cleanup, TAG, "Firmware size is 0");
    ESP_GOTO_ON_FALSE(send_ack(client_sock), ESP_FAIL, cleanup, TAG, "ACK failed");
    vTaskDelay(1);

    ESP_GOTO_ON_FALSE(read_all(client_sock, rx_buffer) == sizeof(md5_expected),
                      ESP_ERR_INVALID_SIZE, cleanup, TAG, "No firmware MD5");
    memcpy(md5_expected, rx_buffer, sizeof(md5_expected));
    ESP_GOTO_ON_FALSE(send_ack(client_sock), ESP_FAIL, cleanup, TAG, "ACK failed");
    vTaskDelay(1);

    OTA_LOG_STEP(2);

    update_partition = esp_ota_get_next_update_partition(NULL);
    vTaskDelay(1);

    ESP_GOTO_ON_ERROR(esp_ota_begin(update_partition, firmware_size, &ota_handle), cleanup, TAG,
                      "Failed to begin OTA update");
    vTaskDelay(1);

    // Initialize PSA Crypto (if not already initialized)
    psa_crypto_init();
    vTaskDelay(1);

    // Initialize MD5 hash operation
    ESP_GOTO_ON_FALSE(psa_hash_setup(&hash_op, PSA_ALG_MD5) == PSA_SUCCESS, ESP_FAIL, cleanup, TAG,
                      "Failed to setup MD5 hash");
    vTaskDelay(1);

    // Ready for image transmission
    OTA_LOG_STEP(3); // Write

    uint32_t total_read_bytes = 0;
    uint16_t read_bytes = -1;
    uint8_t last_percent_reported = -1;
    uint16_t chunk_count = 0;

    while (read_bytes) {
        read_bytes = read_all(client_sock, rx_buffer);
        total_read_bytes += read_bytes;
        chunk_count++;

        int percent = (100 * total_read_bytes) / firmware_size;
        if (percent / 10 != last_percent_reported / 10) {
            ESP_LOGI(TAG, "Progress: %d%% (%" PRIu32 "/%" PRIu32 " bytes)", percent,
                     total_read_bytes, firmware_size);
            last_percent_reported = percent;
        }

        ESP_GOTO_ON_ERROR(esp_ota_write(ota_handle, rx_buffer, read_bytes), cleanup, TAG,
                          "Failed to write OTA data chunk (%d bytes)", read_bytes);
        psa_hash_update(&hash_op, rx_buffer, read_bytes);

        // Yield to watchdog every 10 chunks (~10KB) instead of every chunk
        if (chunk_count % 10 == 0) {
            vTaskDelay(1);
        }
    }

    // The client half-closes the socket once the last byte is out, so a failed ACK here only means
    // it stopped listening. The image is already complete, so finish it either way.
    send_ack(client_sock);

    OTA_LOG_STEP(4);

    ret = esp_ota_end(ota_handle);
    ota_handle = 0;
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Finalizing OTA update failed");
    vTaskDelay(1);

    uint8_t md5_calc[MD5_SIZE];
    size_t hash_length;
    psa_status_t status = psa_hash_finish(&hash_op, md5_calc, sizeof(md5_calc), &hash_length);
    ESP_GOTO_ON_FALSE(status == PSA_SUCCESS && hash_length == MD5_SIZE, ESP_FAIL, cleanup, TAG,
                      "Failed to finalize MD5: status=%ld, len=%zu", (long)status, hash_length);
    vTaskDelay(1);

    // Compare calculated MD5 with expected
    ESP_GOTO_ON_FALSE(!memcmp(md5_calc, md5_expected, sizeof(md5_expected)), ESP_FAIL, cleanup, TAG,
                      "MD5 mismatch");

    ESP_GOTO_ON_ERROR(esp_ota_set_boot_partition(update_partition), cleanup, TAG,
                      "Setting new boot partition failed");

    ota_update_in_progress = false;

    OTA_LOG_STEP(5); // Reboot
    esp_safe_restart();

cleanup:
    if (ota_handle)
        esp_ota_end(ota_handle);
    psa_hash_abort(&hash_op);
    ota_update_in_progress = false;
}

void tcp_ota_task(void *args) {

    struct sockaddr_storage dest_addr;

    struct sockaddr_in6 *dest_addr_ip6 = (struct sockaddr_in6 *)&dest_addr;
    memset(dest_addr_ip6, 0, sizeof(*dest_addr_ip6));
    dest_addr_ip6->sin6_family = AF_INET6;
    dest_addr_ip6->sin6_port = htons(ota_listen_port);
    dest_addr_ip6->sin6_addr = in6addr_any;

    ota_listen_sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_IPV6);
    if (ota_listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket. System error code: %d", errno);
        ota_listen_sock = -1;
        ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(ota_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    int v6only = 0;
    setsockopt(ota_listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

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

        char addr_str[46];
        void *sin_addr = source_addr.ss_family == AF_INET6
                             ? (void *)&((struct sockaddr_in6 *)&source_addr)->sin6_addr
                             : (void *)&((struct sockaddr_in *)&source_addr)->sin_addr;
        inet_ntop(source_addr.ss_family, sin_addr, addr_str, sizeof(addr_str));
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
