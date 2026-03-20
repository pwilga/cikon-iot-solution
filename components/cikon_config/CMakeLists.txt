# If the private config header (config_fields_private.h) does not exist,
# it is automatically copied from the example file (config_fields_private.h.example)
# to the device profile config directory.
# This makes configuration and editing easy, while keeping sensitive data (e.g. passwords)
# outside of version control and only in your local project.
set(PRIV_FIELDS_HEADER "config_fields_private.h")
set(PRIV_FIELDS_HEADER_EXAMPLE "${PRIV_FIELDS_HEADER}.example")

# Use DEVICE_CONFIG_DIR if defined, otherwise fallback to PROJECT_DIR/config/cikon_config
idf_build_get_property(DEVICE_CONFIG_DIR DEVICE_CONFIG_DIR)
message(STATUS "cikon_config DEBUG: DEVICE_CONFIG_DIR='${DEVICE_CONFIG_DIR}'")
if(DEVICE_CONFIG_DIR AND EXISTS "${DEVICE_CONFIG_DIR}")
    set(PROJECT_CONFIG_DIR "${DEVICE_CONFIG_DIR}/cikon_config")
    message(STATUS "cikon_config: Using device profile config dir")
else()
    set(PROJECT_CONFIG_DIR "${PROJECT_DIR}/config/cikon_config")
    message(STATUS "cikon_config: Using fallback config dir")
endif()

set(FALLBACK_DIR "${CMAKE_CURRENT_LIST_DIR}/include")

if (EXISTS "${PROJECT_CONFIG_DIR}/${PRIV_FIELDS_HEADER}")
    message(STATUS "cikon_config: using project's private config: ${PROJECT_CONFIG_DIR}/${PRIV_FIELDS_HEADER}")
elseif (EXISTS "${FALLBACK_DIR}/${PRIV_FIELDS_HEADER_EXAMPLE}")
    file(MAKE_DIRECTORY "${PROJECT_CONFIG_DIR}")
    configure_file("${FALLBACK_DIR}/${PRIV_FIELDS_HEADER_EXAMPLE}"
                   "${PROJECT_CONFIG_DIR}/${PRIV_FIELDS_HEADER}" COPYONLY)
    message(WARNING "cikon_config: created ${PROJECT_CONFIG_DIR}/${PRIV_FIELDS_HEADER} from example. "
                    "Edit this file to override defaults.")
else()
    message(FATAL_ERROR
        "cikon_config: missing both ${PROJECT_CONFIG_DIR}/${PRIV_FIELDS_HEADER} and "
        "${FALLBACK_DIR}/${PRIV_FIELDS_HEADER_EXAMPLE}")
endif()


idf_component_register(
    SRCS
        "config_manager.c"
    INCLUDE_DIRS
        "include" "${PROJECT_CONFIG_DIR}"
    PRIV_REQUIRES
        nvs_flash
)

message(STATUS "cikon_config: using private includes from ${PROJECT_CONFIG_DIR}")
