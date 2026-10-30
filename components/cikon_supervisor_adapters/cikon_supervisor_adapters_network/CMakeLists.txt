# Only the enabled uplinks are built. Their headers (esp_wifi, esp_eth, openthread) come with
# cikon_wifi / cikon_ethernet / cikon_thread_common, pulled in idf_component.yml only for that uplink
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
        esp_event
        esp_netif
        esp_netif_stack
        lwip
)
