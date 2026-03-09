# ESP-NOW adapter: only enable for IDF 6.0+ (5.4 compatibility work in progress)
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    idf_component_register(
        SRCS "esp_now_rx.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES esp_event
    )
endif()
