idf_component_register(
    SRCS
        "ds18b20.c"
    INCLUDE_DIRS
        "include"
)

# X-Macro Pattern for Dynamic HA Entity Generation
# =================================================
# This section converts DS18B20_SENSOR_NAMES from Kconfig into compile-time HA metadata.
# Only runs when CONFIG_MQTT_ENABLE_HA_DISCOVERY is enabled (zero overhead when disabled).
#
# Flow:
# 1. Parse DS18B20_SENSOR_NAMES string (e.g., "Living Room,Bedroom,Kitchen")
# 2. Generate X-Macro invocations: HA_ENTITY_ENTRY("Living Room") HA_ENTITY_ENTRY("Bedroom") ...
# 3. Inject as -DHA_ENTITY_LIST=... compiler flag
# 4. In ds18b20.c: #define HA_ENTITY_ENTRY(name) {...} expands HA_ENTITY_LIST into static const array
#
# This allows:
# - Runtime configuration (user-friendly string in Kconfig)
# - Compile-time metadata (static const ha_metadata_t, no malloc)
# - Zero coupling (no dependency on cikon_mqtt component)
if(CONFIG_MQTT_ENABLE_HA_DISCOVERY)
    # Parse DS18B20_SENSOR_NAMES from Kconfig
    string(REPLACE "," ";" SENSOR_NAME_LIST "${CONFIG_DS18B20_SENSOR_NAMES}")

    # Build X-Macro definition - pad to MAX_SENSORS with fallback names
    set(SENSOR_ENTITIES_MACRO "")
    set(idx 0)
    while(idx LESS ${CONFIG_DS18B20_MAX_SENSORS})
        list(LENGTH SENSOR_NAME_LIST name_count)
        if(idx LESS name_count)
            list(GET SENSOR_NAME_LIST ${idx} name)
        else()
            set(name "temp${idx}")
        endif()
        string(APPEND SENSOR_ENTITIES_MACRO "HA_ENTITY_ENTRY(\"${name}\") ")
        math(EXPR idx "${idx} + 1")
    endwhile()

    # Inject as compile definition
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        HA_ENTITY_LIST=${SENSOR_ENTITIES_MACRO}
    )
endif()
