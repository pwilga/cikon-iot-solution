set(SRCS "http_server.c")
if(CONFIG_HTTP_ENABLE_WEBDAV)
    list(APPEND SRCS "webdav.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_https_server cikon_certs
)

file(GLOB PAGES_FILES "${CMAKE_CURRENT_SOURCE_DIR}/pages/*")
file(COPY ${PAGES_FILES} DESTINATION ${CMAKE_BINARY_DIR}/cikon_http_fs/www)
littlefs_create_partition_image(littlefs ${CMAKE_BINARY_DIR}/cikon_http_fs FLASH_IN_PROJECT)
