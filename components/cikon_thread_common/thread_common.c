#include "thread_common.h"

#include <string.h>

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/dataset.h"
#include "openthread/link.h"
#include "openthread/thread.h"

#define TAG "cikon:thread"

bool thread_dataset_parse_hex(const char *hex, otOperationalDatasetTlvs *out) {
    size_t hex_len = strlen(hex);
    if (hex_len == 0 || hex_len % 2 != 0 || hex_len / 2 > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < hex_len; i += 2) {
        char byte_str[3] = {hex[i], hex[i + 1], '\0'};
        char *end;
        long val = strtol(byte_str, &end, 16);
        if (*end != '\0') {
            return false;
        }
        out->mTlvs[i / 2] = (uint8_t)val;
    }
    out->mLength = (uint8_t)(hex_len / 2);
    return true;
}

void thread_log_network_info(void) {
    otInstance *ot = esp_openthread_get_instance();
    if (!ot) {
        return;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);

    const char *role = otThreadDeviceRoleToString(otThreadGetDeviceRole(ot));
    const char *name = otThreadGetNetworkName(ot);
    uint8_t channel = otLinkGetChannel(ot);
    uint16_t pan_id = otLinkGetPanId(ot);
    uint16_t rloc16 = otThreadGetRloc16(ot);
    uint32_t part_id = otThreadGetPartitionId(ot);

    ESP_LOGI(TAG, "%s | %s | ch=%u | pan=0x%04X | rloc=0x%04X | part=%lu", role, name, channel,
             pan_id, rloc16, (unsigned long)part_id);

    esp_openthread_lock_release();
}

#if CONFIG_OPENTHREAD_CLI
#include "esp_console.h"
#include "esp_ot_cli_extension.h"

void thread_console_start(const char *prompt) {
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = prompt;
    repl_config.max_cmdline_length = 256;
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &repl_config, &repl));
#endif
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void thread_cli_commands_init(void) {
    esp_cli_custom_command_init();
}
#endif
