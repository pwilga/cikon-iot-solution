set(srcs "thread_common.c" "thread_tele.c")

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
        lwip
)

# The Thread page goes where Thread runs and the HTTP component is built: staged into the shared
# LittleFS image next to the dashboard, gzipped the same way
idf_build_get_property(fs_dir LITTLEFS_IMAGE_DIR)
if(fs_dir AND CONFIG_HTTP_STACK_SIZE AND
   (CONFIG_ENABLE_SUPERVISOR_THREAD_BORDER_ROUTER OR CONFIG_NETWORK_UPLINK_THREAD))
    file(GLOB PAGES_FILES "${CMAKE_CURRENT_SOURCE_DIR}/pages/*")
    file(COPY ${PAGES_FILES} DESTINATION "${fs_dir}/www")

    find_program(GZIP_EXECUTABLE gzip)
    if(GZIP_EXECUTABLE)
        foreach(page_file ${PAGES_FILES})
            get_filename_component(page_name "${page_file}" NAME)
            execute_process(COMMAND ${GZIP_EXECUTABLE} -f9 "${fs_dir}/www/${page_name}")
        endforeach()
    endif()
endif()
