#include "esp_wifi.h"

#include "json_parser.h"
#include "network_ap.h"
#include "wifi.h"

// Local service access point next to the uplink. Not an uplink itself: it only adds an interface
// the already running services answer on.
#if CONFIG_NETWORK_UPLINK_WIFI

static void network_ap_handler(const char *args_json_str) {
    logic_state_t state = json_str_as_logic_state(args_json_str);

    if (state == STATE_TOGGLE) {
        wifi_mode_t mode = WIFI_MODE_NULL;
        esp_wifi_get_mode(&mode);
        state = (mode == WIFI_MODE_APSTA) ? STATE_OFF : STATE_ON;
    }

    if (state == STATE_ON) {
        wifi_ap_enable();
    } else {
        wifi_ap_disable();
    }
}

const command_entry_t network_ap_commands[] = {
    {"ap", "Control WiFi access point next to the uplink (on/off)", network_ap_handler},
    {NULL, NULL, NULL}};

#endif
