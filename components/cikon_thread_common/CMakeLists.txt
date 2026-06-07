idf_component_register(
    SRCS "thread_common.c"
    INCLUDE_DIRS "include"
    REQUIRES
        openthread
    PRIV_REQUIRES
        console
)
