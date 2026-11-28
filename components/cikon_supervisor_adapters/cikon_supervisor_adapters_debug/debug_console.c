#include "debug_console.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_console.h"

#include "cmnd.h"
#include "config_manager.h"

// Hands a supervisor command over: cmnd <name> [json]. The console splits its line on spaces, so
// the JSON goes without them. A command without arguments gets "null", as it does over the network
static int debug_console_cmnd(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        printf("Usage: cmnd <name> [json without spaces]\n");
        return 1;
    }
    cmnd_submit(argv[1], argc == 3 ? argv[2] : "null");
    return 0;
}

// The supervisor may init the adapter again, the console starts only once
void debug_console_start(void) {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;

    static char prompt[20];
    snprintf(prompt, sizeof(prompt), "%s>", config_get()->dev_name);

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
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &repl_config, &repl));
#endif
    const esp_console_cmd_t cmnd = {
        .command = "cmnd",
        .help = "Run a supervisor command: cmnd <name> [json without spaces], cmnd help lists them",
        .func = debug_console_cmnd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmnd));

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
