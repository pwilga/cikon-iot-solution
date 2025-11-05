idf_component_register(
    SRCS
        "platform_services.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        driver nvs_flash esp_event
)
