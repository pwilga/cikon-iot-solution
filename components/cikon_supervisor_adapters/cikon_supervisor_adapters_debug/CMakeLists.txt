set(srcs "debug.c")

if(CONFIG_DEBUG_CONSOLE)
    list(APPEND srcs "debug_console.c")
endif()

idf_component_register(
    SRCS
        ${srcs}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private_include"
    PRIV_REQUIRES
        console
        spi_flash
        app_update
        espcoredump
        esp_netif
        lwip
)
