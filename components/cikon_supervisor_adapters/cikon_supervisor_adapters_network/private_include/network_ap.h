#pragma once

#include "cmnd.h"

#if CONFIG_NETWORK_UPLINK_WIFI
// "ap" command: on / off / toggle the access point next to the uplink
extern const command_entry_t network_ap_commands[];
#endif
