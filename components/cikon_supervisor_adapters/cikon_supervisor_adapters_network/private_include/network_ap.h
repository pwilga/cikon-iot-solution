#pragma once

#include "cmnd.h"
#include "esp_netif_ip_addr.h"

#if CONFIG_NETWORK_UPLINK_WIFI
// "ap" command: on / off / toggle the access point next to the uplink
extern const command_entry_t network_ap_commands[];

// GET /scan for the WiFi setup portal - the HTTP server keeps it across its restarts
void network_ap_http_register(void);

// DNS on the AP: every A query gets ap_ip, so phones find their connectivity check on the device
void network_ap_dns_start(esp_ip4_addr_t ap_ip);
// Releases everything the DNS holds
void network_ap_dns_stop(void);
#endif
