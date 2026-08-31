# LIGHT_HAS_ADDRESSABLE Detection
# ================================
# Scans LIGHT_GPIO_LIST for a role-N (addressable WS2812/SK6812 strip) channel token, so
# the CH_ADDRESSABLE code path in light.c (and its espressif/led_strip usage) is only
# compiled in when actually configured - devices without a strip don't pay for it. Computed
# before idf_component_register() (below) since it decides whether light_effects.c is part
# of SRCS.
string(REPLACE "," ";" LIGHT_ADDR_ITEMS "${CONFIG_LIGHT_GPIO_LIST}")
set(LIGHT_HAS_ADDRESSABLE FALSE)
# Largest "x<led count>" seen across all N-role channels - sizes the effects engine's per-pixel
# scratch buffer (light_effect_state_t.buf) exactly, computed here rather than guessed, the
# same way LIGHT_HAS_ADDRESSABLE itself is derived from the config string below.
set(LIGHT_EFFECTS_MAX_LEDS 0)
foreach(item ${LIGHT_ADDR_ITEMS})
    string(REPLACE ":" ";" LIGHT_ADDR_TUPLE "${item}")
    list(GET LIGHT_ADDR_TUPLE 0 light_addr_channels)
    if(light_addr_channels MATCHES "(^|\\+)[Nn][0-9]")
        set(LIGHT_HAS_ADDRESSABLE TRUE)
    endif()
    if(light_addr_channels MATCHES "[Nn][0-9]+[Xx]([0-9]+)")
        if(CMAKE_MATCH_1 GREATER LIGHT_EFFECTS_MAX_LEDS)
            set(LIGHT_EFFECTS_MAX_LEDS "${CMAKE_MATCH_1}")
        endif()
    endif()
endforeach()

# LIGHT_EFFECTS_BUILD - opt-in animation task for addressable strips
# =====================================================================
# Only compiled in when the device both has a role-N channel (LIGHT_HAS_ADDRESSABLE, above)
# and the user hasn't opted out (CONFIG_LIGHT_ENABLE_EFFECTS). A device with no addressable
# strip never pays for this regardless of the Kconfig setting.
set(LIGHT_SRCS "light.c")
if(CONFIG_LIGHT_ENABLE_EFFECTS AND LIGHT_HAS_ADDRESSABLE)
    list(APPEND LIGHT_SRCS "light_effects.c")
endif()

idf_component_register(
    SRCS
        ${LIGHT_SRCS}
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_driver_ledc
        esp_driver_gpio
        nvs_flash
)

if(LIGHT_HAS_ADDRESSABLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIGHT_HAS_ADDRESSABLE=1)
endif()
if(CONFIG_LIGHT_ENABLE_EFFECTS AND LIGHT_HAS_ADDRESSABLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIGHT_EFFECTS_BUILD=1)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIGHT_EFFECTS_MAX_LEDS=${LIGHT_EFFECTS_MAX_LEDS})
endif()

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
