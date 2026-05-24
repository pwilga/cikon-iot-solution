# CRITICAL: ONLY internal cikon_* components in REQUIRES
# NO external components (espressif/*) - they go in idf_component.yml

idf_component_register(
    SRCS "inet_ethernet.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_eth
        esp_netif
        esp_event
)
