# Only the enabled uplinks are built. Requirements can't depend on Kconfig, so they stay listed
set(srcs "network.c")

if(CONFIG_NETWORK_UPLINK_WIFI)
    list(APPEND srcs "uplink_wifi.c" "network_ap.c")
endif()

if(CONFIG_NETWORK_UPLINK_ETHERNET)
    list(APPEND srcs "uplink_ethernet.c")
endif()

if(CONFIG_NETWORK_UPLINK_THREAD)
    list(APPEND srcs "uplink_thread.c")
endif()

idf_component_register(
    SRCS
        ${srcs}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private_include"
    PRIV_REQUIRES
        esp_eth
        esp_wifi
        esp_netif_stack
        lwip
        openthread
)
