idf_component_register(
    SRCS
        "led.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_driver_ledc
)

# X-Macro Pattern for Dynamic HA Entity Generation
# =================================================
# This section converts LED_GPIO_LIST from Kconfig into compile-time HA metadata.
# Only runs when CONFIG_MQTT_ENABLE_HA_DISCOVERY is enabled (zero overhead when disabled).
#
# Flow:
# 1. Parse LED_GPIO_LIST string (e.g., "25:Left Strip,27:Right Strip")
# 2. Generate X-Macro invocations: HA_ENTITY_ENTRY(25,"Left Strip") HA_ENTITY_ENTRY(27,"Right Strip")
# 3. Inject as -DLIST_ENTITIES=... compiler flag
# 4. In led.c: #define HA_ENTITY_ENTRY(gpio, name) {...} expands LIST_ENTITIES into static const array
#
# This allows:
# - Runtime configuration (user-friendly string in Kconfig)
# - Compile-time metadata (static const ha_metadata_t, no malloc)
# - Zero coupling (no dependency on cikon_mqtt component)
if(CONFIG_MQTT_ENABLE_HA_DISCOVERY)
    # Parse LED_GPIO_LIST from Kconfig
    string(REPLACE "," ";" LED_LIST_ITEMS "${CONFIG_LED_GPIO_LIST}")

    # Build X-Macro definition
    set(LED_ENTITIES_MACRO "")
    foreach(item ${LED_LIST_ITEMS})
        string(REPLACE ":" ";" GPIO_NAME_PAIR "${item}")
        list(GET GPIO_NAME_PAIR 0 gpio)
        list(LENGTH GPIO_NAME_PAIR pair_length)
        if(pair_length GREATER 1)
            list(GET GPIO_NAME_PAIR 1 name)
        else()
            set(name "led${gpio}")
        endif()
        string(APPEND LED_ENTITIES_MACRO "HA_ENTITY_ENTRY(${gpio},\"${name}\") ")
    endforeach()

    # Inject as compile definition
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        HA_ENTITY_LIST=${LED_ENTITIES_MACRO}
    )
endif()
