idf_component_register(
    SRCS
        "switch.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_driver_gpio
)

# X-Macro Pattern for Dynamic cmnd Trampoline Generation
# =========================================================
# One cmnd is registered per configured switch (see switch.c), each needing its own thin
# handler that closes over a fixed index into switches[] (command_handler_t carries no
# context). Instead of hand-writing/hardcoding those, count the entries in
# SWITCH_GPIO_LIST here (CMake has real string-list processing, the C preprocessor doesn't)
# and inject exactly that many X(n) tokens.
string(REPLACE "," ";" SWITCH_CMND_LIST_ITEMS "${CONFIG_SWITCH_GPIO_LIST}")
set(SWITCH_CMND_LIST "")
set(switch_cmnd_idx 0)
foreach(item ${SWITCH_CMND_LIST_ITEMS})
    string(APPEND SWITCH_CMND_LIST "X(${switch_cmnd_idx}) ")
    math(EXPR switch_cmnd_idx "${switch_cmnd_idx} + 1")
endforeach()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SWITCH_CMND_LIST=${SWITCH_CMND_LIST}
)

# X-Macro Pattern for Dynamic HA Entity Generation
# =================================================
# Same approach as cikon_supervisor_adapters_led/CMakeLists.txt, adapted for the
# gpio:active_level:name (3-field) shape of SWITCH_GPIO_LIST instead of led's gpio:name (2-field).
if(CONFIG_MQTT_ENABLE_HA_DISCOVERY)
    string(REPLACE "," ";" SWITCH_LIST_ITEMS "${CONFIG_SWITCH_GPIO_LIST}")

    set(SWITCH_ENTITIES_MACRO "")
    foreach(item ${SWITCH_LIST_ITEMS})
        string(REPLACE ":" ";" SWITCH_TUPLE "${item}")
        list(GET SWITCH_TUPLE 0 gpio)
        list(LENGTH SWITCH_TUPLE tuple_length)
        if(tuple_length GREATER 2)
            list(GET SWITCH_TUPLE 2 name)
        else()
            set(name "gpio${gpio}")
        endif()
        string(APPEND SWITCH_ENTITIES_MACRO "HA_ENTITY_ENTRY(${gpio},\"${name}\") ")
    endforeach()

    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        HA_ENTITY_LIST=${SWITCH_ENTITIES_MACRO}
    )
endif()
