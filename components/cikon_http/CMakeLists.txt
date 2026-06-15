idf_component_register(
    SRCS "http_server.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server cikon_supervisor
)

file(GLOB PAGES_FILES "${CMAKE_CURRENT_SOURCE_DIR}/pages/*")
file(COPY ${PAGES_FILES} DESTINATION ${CMAKE_BINARY_DIR}/cikon_http_fs/www)
littlefs_create_partition_image(littlefs ${CMAKE_BINARY_DIR}/cikon_http_fs FLASH_IN_PROJECT)
