idf_component_register(
    SRCS
        "light.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_driver_ledc
        esp_driver_gpio
)

# X-Macro Pattern for Dynamic cmnd Trampoline Generation
# =========================================================
# One cmnd is registered per configured light (see light.c), each needing its own thin
# handler that closes over a fixed index into lights[] (command_handler_t carries no
# context). Instead of hand-writing/hardcoding those, count the entries in
# LIGHT_GPIO_LIST here (CMake has real string-list processing, the C preprocessor doesn't)
# and inject exactly that many X(n) tokens.
string(REPLACE "," ";" LIGHT_CMND_LIST_ITEMS "${CONFIG_LIGHT_GPIO_LIST}")
set(LIGHT_CMND_LIST "")
set(light_cmnd_idx 0)
foreach(item ${LIGHT_CMND_LIST_ITEMS})
    string(APPEND LIGHT_CMND_LIST "X(${light_cmnd_idx}) ")
    math(EXPR light_cmnd_idx "${light_cmnd_idx} + 1")
endforeach()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LIGHT_CMND_LIST=${LIGHT_CMND_LIST}
)

# X-Macro Pattern for Dynamic HA Entity Generation
# =================================================
# Same approach as cikon_supervisor_adapters_switch/CMakeLists.txt, adapted for the
# "<role><gpio>[+<role><gpio>...][:name]" shape of LIGHT_GPIO_LIST.
if(CONFIG_MQTT_ENABLE_HA_DISCOVERY)
    string(REPLACE "," ";" LIGHT_LIST_ITEMS "${CONFIG_LIGHT_GPIO_LIST}")

    set(LIGHT_ENTITIES_MACRO "")
    set(light_ha_idx 0)
    foreach(item ${LIGHT_LIST_ITEMS})
        string(REPLACE ":" ";" LIGHT_TUPLE "${item}")
        list(LENGTH LIGHT_TUPLE tuple_length)
        if(tuple_length GREATER 1)
            list(GET LIGHT_TUPLE 1 name)
        else()
            set(name "light${light_ha_idx}")
        endif()
        string(APPEND LIGHT_ENTITIES_MACRO "HA_ENTITY_ENTRY(\"${name}\") ")
        math(EXPR light_ha_idx "${light_ha_idx} + 1")
    endforeach()

    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        HA_ENTITY_LIST=${LIGHT_ENTITIES_MACRO}
    )
endif()
