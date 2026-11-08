# Only the enabled uplinks are built. Their headers (esp_wifi, esp_eth, openthread) come with
# cikon_wifi / cikon_ethernet / cikon_thread, pulled in idf_component.yml only for that uplink
set(srcs "network.c" "services/network_services.c" "services/network_internet.c")

if(CONFIG_MQTT_ENABLE_HA_DISCOVERY)
    list(APPEND srcs "services/network_ha.c")
endif()

if(CONFIG_NETWORK_UPLINK_WIFI)
    list(APPEND srcs "uplinks/uplink_wifi.c" "uplinks/network_ap.c")
endif()

if(CONFIG_NETWORK_UPLINK_ETHERNET)
    list(APPEND srcs "uplinks/uplink_ethernet.c")
endif()

if(CONFIG_NETWORK_UPLINK_THREAD)
    list(APPEND srcs "uplinks/uplink_thread.c")
endif()

# This adapter's settings are the profile's network.yaml laid over network.defaults.yaml, turned
# into a field list at configure time by tools/network_cikon_config_gen.py and handed to
# cikon_config. A profile without a network.yaml gets the defaults as they are. The early pass that
# only collects requirements has no build directory to write to.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(DEVICE_CONFIG_DIR DEVICE_CONFIG_DIR)
    set(NETWORK_DEFAULTS_YAML "${CMAKE_CURRENT_LIST_DIR}/network.defaults.yaml")
    set(NETWORK_PROFILE_YAML "${DEVICE_CONFIG_DIR}/cikon_network/network.yaml")
    set(NETWORK_GEN "${CMAKE_CURRENT_LIST_DIR}/tools/network_cikon_config_gen.py")
    get_property(CONFIG_FIELDS_DIR GLOBAL PROPERTY CIKON_CONFIG_FIELDS_DIR)
    set(NETWORK_FIELDS_HEADER "${CONFIG_FIELDS_DIR}/network_fields.h")

    # Re-run when the profile, the defaults or the generator change, or a changed network.yaml
    # would build the old field list
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${NETWORK_DEFAULTS_YAML}" "${NETWORK_PROFILE_YAML}" "${NETWORK_GEN}")

    # ESP-IDF's own interpreter, not the system one: the generator needs PyYAML, which IDF installs
    # into that environment
    idf_build_get_property(python PYTHON)
    execute_process(
        COMMAND "${python}" "${NETWORK_GEN}" "${NETWORK_DEFAULTS_YAML}" "${NETWORK_PROFILE_YAML}"
                --name network --out-header "${NETWORK_FIELDS_HEADER}"
        OUTPUT_VARIABLE NETWORK_GEN_OUTPUT
        ERROR_VARIABLE NETWORK_GEN_ERROR
        RESULT_VARIABLE NETWORK_GEN_RESULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE)

    if(NOT NETWORK_GEN_RESULT EQUAL 0)
        message(FATAL_ERROR "network: ${NETWORK_GEN_ERROR}")
    endif()

    # A service left out of network.yaml does not start, and nothing else says so
    message(STATUS "network: ${NETWORK_GEN_OUTPUT}")

    cikon_config_add_fields(network_fields.h NETWORK_FIELDS)
endif()

idf_component_register(
    SRCS
        ${srcs}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private_include"
    PRIV_REQUIRES
        esp_event
        esp_netif
        esp_netif_stack
        lwip
        openthread
)
