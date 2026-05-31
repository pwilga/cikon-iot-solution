# CRITICAL: ONLY IDF built-in components in REQUIRES
# NO external/registry components - they go in idf_component.yml

idf_component_register(
    SRCS "thread_br_adapter.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_netif
        esp_openthread
        openthread
        esp_driver_uart
)
