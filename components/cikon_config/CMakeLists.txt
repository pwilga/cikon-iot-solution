idf_build_get_property(DEVICE_CONFIG_DIR DEVICE_CONFIG_DIR)
if(NOT DEVICE_CONFIG_DIR)
    message(FATAL_ERROR "cikon_config: DEVICE_CONFIG_DIR is not set, so there is no profile to "
                        "name the device after. The project root is expected to set it with "
                        "idf_build_set_property(DEVICE_CONFIG_DIR ...).")
endif()

# config_manager.h expands one list of settings, config_fields.h. A component with settings of its
# own writes a header that defines <MACRO>(STR, U8, U16, U32, U64) into CIKON_CONFIG_FIELDS_DIR,
# the directory config_fields.h is in, and names it with cikon_config_add_fields(). The list is
# pasted in by that name alone. This component has to be configured before the one that calls it,
# which its dependency in idf_component.yml makes sure of.
set_property(GLOBAL PROPERTY CIKON_CONFIG_FIELDS_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set_property(GLOBAL PROPERTY CIKON_CONFIG_FIELDS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/config_fields.h")
set_property(GLOBAL PROPERTY CIKON_CONFIG_FIELDS "")

function(cikon_config_add_fields header macro)
    get_property(fields GLOBAL PROPERTY CIKON_CONFIG_FIELDS)
    list(APPEND fields "${header}|${macro}")
    list(REMOVE_DUPLICATES fields)
    set_property(GLOBAL PROPERTY CIKON_CONFIG_FIELDS "${fields}")

    set(includes "")
    set(expansion "")
    foreach(field ${fields})
        string(REPLACE "|" ";" parts "${field}")
        list(GET parts 0 field_header)
        list(GET parts 1 field_macro)
        string(APPEND includes "#include \"${field_header}\"\n")
        string(APPEND expansion " \\\n    ${field_macro}(STR, U8, U16, U32, U64)")
    endforeach()

    # Written only when its content changes, so a configure run does not rebuild everything
    get_property(fields_header GLOBAL PROPERTY CIKON_CONFIG_FIELDS_HEADER)
    file(CONFIGURE OUTPUT "${fields_header}" @ONLY
         CONTENT "#pragma once\n\n@includes@\n#define CONFIG_FIELDS(STR, U8, U16, U32, U64)@expansion@\n")
endfunction()

# The early pass that only collects requirements has no build directory to write to
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # A device starts out named after its profile (devices/<name>/config); setconf renames it
    # from there on
    get_filename_component(DEVICE_DIR "${DEVICE_CONFIG_DIR}" DIRECTORY)
    get_filename_component(DEVICE_NAME "${DEVICE_DIR}" NAME)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/device_fields.h"
         CONTENT "#define DEVICE_FIELDS(STR, U8, U16, U32, U64) STR(dev_name, 16, \"${DEVICE_NAME}\")\n")
    cikon_config_add_fields(device_fields.h DEVICE_FIELDS)
endif()

idf_component_register(
    SRCS
        "config_manager.c"
    INCLUDE_DIRS
        "include" "${CMAKE_CURRENT_BINARY_DIR}"
    PRIV_REQUIRES
        nvs_flash
)
