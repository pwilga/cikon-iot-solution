idf_component_register(
    SRCS "cikon_esp_now.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_event esp_netif
)
