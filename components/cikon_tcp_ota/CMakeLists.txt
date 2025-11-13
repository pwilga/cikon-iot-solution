idf_component_register(
    SRCS
       "tcp_ota.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        lwip app_update mbedtls cikon_core
)
