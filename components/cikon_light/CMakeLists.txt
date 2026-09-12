# The lights this device drives come from a per-profile lights.yaml, turned into a C table here at
# configure time by tools/lights_gen.py. The script is also what answers the three questions this
# file used to answer with regexes over a config string: whether any channel is PWM, whether any is
# an addressable strip, and how long the longest strip is.
idf_build_get_property(DEVICE_CONFIG_DIR DEVICE_CONFIG_DIR)
if(NOT DEVICE_CONFIG_DIR)
    message(FATAL_ERROR "cikon_light: DEVICE_CONFIG_DIR is not set, so there is nowhere to look "
                        "for lights.yaml. The project root is expected to set it with "
                        "idf_build_set_property(DEVICE_CONFIG_DIR ...).")
endif()

set(LIGHT_CONFIG_YAML "${DEVICE_CONFIG_DIR}/cikon_light/lights.yaml")
set(LIGHT_CONFIG_GEN "${CMAKE_CURRENT_LIST_DIR}/tools/lights_gen.py")

if(NOT EXISTS "${LIGHT_CONFIG_YAML}")
    message(FATAL_ERROR
        "cikon_light: no lights.yaml for this device profile.\n"
        "  Expected:\n"
        "    ${LIGHT_CONFIG_YAML}\n"
        "\n"
        "  Every entry in that file is a GPIO this firmware will drive, so there is no safe\n"
        "  default to fall back on.\n"
        "\n"
        "    - this device does have lights: copy the template and edit it\n"
        "        cp ${CMAKE_CURRENT_LIST_DIR}/tools/lights.yaml.example \\\n"
        "           ${LIGHT_CONFIG_YAML}\n"
        "\n"
        "    - this device has no lights: set CONFIG_ENABLE_SUPERVISOR_LIGHT=n\n")
endif()

# Re-run when either the wiring or the generator changes. Without this a changed lights.yaml would
# build the old table, which is the one failure this whole arrangement exists to prevent.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${LIGHT_CONFIG_YAML}" "${LIGHT_CONFIG_GEN}")

set(LIGHT_GENERATED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/light_generated.h")
set(LIGHT_GENERATED_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/light_generated.c")

# ESP-IDF's own interpreter, not the system one: lights_gen.py needs PyYAML, which IDF installs
# into that environment but which a bare system Python is unlikely to have.
idf_build_get_property(python PYTHON)
execute_process(
    COMMAND "${python}" "${LIGHT_CONFIG_GEN}" "${LIGHT_CONFIG_YAML}"
            --out-header "${LIGHT_GENERATED_HEADER}"
            --out-source "${LIGHT_GENERATED_SOURCE}"
    OUTPUT_VARIABLE LIGHT_GEN_OUTPUT
    ERROR_VARIABLE LIGHT_GEN_ERROR
    RESULT_VARIABLE LIGHT_GEN_RESULT
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if(NOT LIGHT_GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "cikon_light: ${LIGHT_GEN_ERROR}")
endif()

# The script prints one KEY=VALUE per line, and foreach walks a ;-separated list - without this
# the whole output stays a single item and only the first key is ever read.
string(REPLACE "\n" ";" LIGHT_GEN_OUTPUT "${LIGHT_GEN_OUTPUT}")
foreach(line ${LIGHT_GEN_OUTPUT})
    if(line MATCHES "^([A-Z_]+)=(.*)$")
        set(LIGHT_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endif()
endforeach()

# The animation task is opt-in and only means anything for a strip: a device with none never pays
# for it whatever the Kconfig setting says.
set(LIGHT_SRCS "light.c" "light_color.c" "${LIGHT_GENERATED_SOURCE}")
if(CONFIG_LIGHT_ENABLE_EFFECTS AND LIGHT_HAS_ADDRESSABLE)
    list(APPEND LIGHT_SRCS "light_effects.c")
endif()

idf_component_register(
    SRCS
        ${LIGHT_SRCS}
    # The generated header is public: besides sizing this component's own tables it carries the
    # per-light command and entity lists the adapter expands. light_internal.h, which has the
    # types and lights[] itself, stays private.
    INCLUDE_DIRS
        "include" "${CMAKE_CURRENT_BINARY_DIR}"
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

if(LIGHT_HAS_PWM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIGHT_HAS_PWM=1)
endif()
if(LIGHT_HAS_ADDRESSABLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIGHT_HAS_ADDRESSABLE=1)
endif()
if(CONFIG_LIGHT_ENABLE_EFFECTS AND LIGHT_HAS_ADDRESSABLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIGHT_EFFECTS_BUILD=1)
endif()
