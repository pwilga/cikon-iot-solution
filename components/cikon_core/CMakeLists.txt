idf_component_register(
    SRCS
        "platform_services.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_driver_gpio nvs_flash esp_event
)
