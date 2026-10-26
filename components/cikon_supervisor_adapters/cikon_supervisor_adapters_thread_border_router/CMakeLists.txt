idf_component_register(
    SRCS "thread_border_router_adapter.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_netif
        lwip
        openthread
        esp_driver_uart
)
