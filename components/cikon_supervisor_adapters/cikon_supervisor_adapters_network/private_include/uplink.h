#pragma once

#include "esp_err.h"

#include "cmnd.h"
#include "tele.h"

// One way of getting the device onto the network. The uplink only brings its interface up and
// reports it through supervisor event bits; the network adapter starts the services on top.
typedef struct {
    const char *name; // reported as tele "link"
    esp_err_t (*start)(void);
    esp_err_t (*stop)(void);
    const command_entry_t *commands; // NULL-terminated, may be NULL
    const tele_entry_t *tele;        // NULL-terminated, may be NULL
} uplink_t;

#if CONFIG_NETWORK_UPLINK_WIFI
extern const uplink_t uplink_wifi;
#endif

// For uplinks that switch modes on their own (WiFi STA <-> AP)
void network_stop_services(void);
