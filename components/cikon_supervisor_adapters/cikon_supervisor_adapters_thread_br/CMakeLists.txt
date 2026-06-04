idf_component_register(
    SRCS "thread_br_adapter.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_netif
        openthread
        esp_driver_uart
        console
)
