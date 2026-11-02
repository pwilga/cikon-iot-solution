set(srcs "thread_common.c")

# A Thread service is built only when OpenThread has it
if(CONFIG_OPENTHREAD_COMMISSIONER)
    list(APPEND srcs "thread_commissioner.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES
        openthread
    PRIV_REQUIRES
        console
)
