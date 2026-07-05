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
endif()
