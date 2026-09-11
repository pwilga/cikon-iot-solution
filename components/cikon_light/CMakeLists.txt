# LIGHT_HAS_ADDRESSABLE / LIGHT_HAS_PWM Detection
# ===============================================
# Scans LIGHT_GPIO_LIST for a role-N (addressable WS2812/SK6812 strip) channel token, so
# the CH_ADDRESSABLE code path in light.c (and its espressif/led_strip usage) is only
# compiled in when actually configured - devices without a strip don't pay for it. Computed
# before idf_component_register() (below) since it decides whether light_effects.c is part
# of SRCS.
#
# The R/G/B/C/W tokens get the same treatment: a device driving only a strip or only a relay
# would otherwise still hold LEDC_TIMER_0, install the fade ISR and carry the duty math.
string(REPLACE "," ";" LIGHT_ADDR_ITEMS "${CONFIG_LIGHT_GPIO_LIST}")
set(LIGHT_HAS_ADDRESSABLE FALSE)
set(LIGHT_HAS_PWM FALSE)
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
    if(light_addr_channels MATCHES "(^|\\+)[RrGgBbCcWw][0-9]")
        set(LIGHT_HAS_PWM TRUE)
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
set(LIGHT_SRCS "light.c" "light_color.c" "light_config.c")
if(CONFIG_LIGHT_ENABLE_EFFECTS AND LIGHT_HAS_ADDRESSABLE)
    list(APPEND LIGHT_SRCS "light_effects.c")
endif()

idf_component_register(
    SRCS
        ${LIGHT_SRCS}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private_include"
    # Not narrowed by LIGHT_HAS_PWM: requirements are collected in an earlier pass than the one
    # that sees CONFIG_*, so the flag isn't known yet here. It costs nothing - what LIGHT_HAS_PWM
    # compiles out is simply never referenced, and the linker drops it.
    PRIV_REQUIRES
        esp_driver_ledc
        esp_driver_gpio
        nvs_flash
)

if(LIGHT_HAS_ADDRESSABLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIGHT_HAS_ADDRESSABLE=1)
endif()
if(LIGHT_HAS_PWM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIGHT_HAS_PWM=1)
endif()
if(CONFIG_LIGHT_ENABLE_EFFECTS AND LIGHT_HAS_ADDRESSABLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIGHT_EFFECTS_BUILD=1)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIGHT_EFFECTS_MAX_LEDS=${LIGHT_EFFECTS_MAX_LEDS})
endif()
