# Check if all required certificate files exist
idf_build_get_property(DEVICE_CERTS_DIR DEVICE_CERTS_DIR)
set(CERTS_AVAILABLE FALSE)
if(DEVICE_CERTS_DIR AND EXISTS "${DEVICE_CERTS_DIR}")
    set(SERVER_CERT "${DEVICE_CERTS_DIR}/ca.pem")
    set(CLIENT_CERT "${DEVICE_CERTS_DIR}/client.pem")
    set(CLIENT_KEY "${DEVICE_CERTS_DIR}/client.key")

    if(EXISTS "${SERVER_CERT}" AND EXISTS "${CLIENT_CERT}" AND EXISTS "${CLIENT_KEY}")
        set(CERTS_AVAILABLE TRUE)
    endif()
endif()

# Embed certificates if all files were found
if(CERTS_AVAILABLE)
    set(CERTS_TEMPLATE ${CMAKE_CURRENT_LIST_DIR}/certs.c.internal)
    set(GENERATED_CERTS_C "${CMAKE_CURRENT_BINARY_DIR}/certs.c")
    configure_file(${CERTS_TEMPLATE} ${GENERATED_CERTS_C} COPYONLY)
    idf_component_register(
        SRCS
            "${GENERATED_CERTS_C}"
        INCLUDE_DIRS
            "include"
        EMBED_TXTFILES
            "${SERVER_CERT}"
            "${CLIENT_CERT}"
            "${CLIENT_KEY}"
    )
else()
    set(CERTS_TEMPLATE ${CMAKE_CURRENT_LIST_DIR}/certs.c.external)
    set(GENERATED_CERTS_C "${CMAKE_CURRENT_BINARY_DIR}/certs.c")
    configure_file(${CERTS_TEMPLATE} ${GENERATED_CERTS_C} COPYONLY)
    idf_component_register(
        SRCS
            "${GENERATED_CERTS_C}"
        INCLUDE_DIRS
            "include"
    )
endif()