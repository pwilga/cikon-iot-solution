idf_component_register(
    SRCS
        "platform_services.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        mdns
    PRIV_REQUIRES
        driver esp_timer nvs_flash
)
