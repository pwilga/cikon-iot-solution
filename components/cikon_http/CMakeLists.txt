set(SRCS "http_server.c")
if(CONFIG_HTTP_ENABLE_WEBDAV)
    list(APPEND SRCS "webdav.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_https_server cikon_certs
)

# Stage web pages into the shared LittleFS image (served from <mount>/www).
# The image itself is created by the top-level project CMakeLists.
idf_build_get_property(fs_dir LITTLEFS_IMAGE_DIR)
if(fs_dir)
    file(GLOB PAGES_FILES "${CMAKE_CURRENT_SOURCE_DIR}/pages/*")
    file(COPY ${PAGES_FILES} DESTINATION "${fs_dir}/www")

    # Replace text assets with their .gz form in the image (partition ships
    # only the compressed copy; plain files only ever appear via ad-hoc
    # WebDAV uploads at runtime, never in the built image). Operates on the
    # copy under fs_dir/www, never touches the source in pages/.
    find_program(GZIP_EXECUTABLE gzip)
    if(GZIP_EXECUTABLE)
        file(GLOB COMPRESS_FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/pages/*.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/pages/*.css"
            "${CMAKE_CURRENT_SOURCE_DIR}/pages/*.js")
        foreach(page_file ${COMPRESS_FILES})
            get_filename_component(page_name "${page_file}" NAME)
            execute_process(COMMAND ${GZIP_EXECUTABLE} -f9 "${fs_dir}/www/${page_name}")
        endforeach()
    endif()
endif()
