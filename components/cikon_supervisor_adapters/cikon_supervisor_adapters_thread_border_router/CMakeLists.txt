idf_component_register(
    SRCS "thread_border_router_adapter.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_netif
        lwip
        openthread
        esp_driver_uart
)

# The Thread page goes only to border routers: staged into the shared LittleFS image next to the
# dashboard (served from <mount>/www), gzipped the same way as cikon_http's pages
idf_build_get_property(fs_dir LITTLEFS_IMAGE_DIR)
if(fs_dir AND CONFIG_ENABLE_SUPERVISOR_THREAD_BORDER_ROUTER)
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
