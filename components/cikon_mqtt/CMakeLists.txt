set(MQTT_SRCS "mqtt.c")

if(CONFIG_MQTT_ENABLE_HA_DISCOVERY)
    list(APPEND MQTT_SRCS "ha.c")
endif()

idf_component_register(
    SRCS ${MQTT_SRCS}
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        mqtt
)