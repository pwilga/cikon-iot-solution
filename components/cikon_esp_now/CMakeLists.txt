# ESP-NOW: only enable for IDF 6.0+ (5.4 compatibility work in progress)
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    idf_component_register(
        SRCS "cikon_esp_now.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES esp_wifi esp_event esp_netif
    )
endif()
