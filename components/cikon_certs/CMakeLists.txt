# Automatically detect the only .key file in certs/ in main project directory
file(GLOB KEY_FILES "${PROJECT_DIR}/certs/*.key")

if(KEY_FILES)
    list(GET KEY_FILES 0 KEY_FILE)
    get_filename_component(CLIENT_NAME ${KEY_FILE} NAME_WE)
    set(CERTS_TEMPLATE ${CMAKE_CURRENT_LIST_DIR}/certs.c.internal)
    set(SERVER_CERT "${PROJECT_DIR}/certs/ca.pem")
    set(CLIENT_CERT "${PROJECT_DIR}/certs/${CLIENT_NAME}.pem")
    set(CLIENT_KEY "${PROJECT_DIR}/certs/${CLIENT_NAME}.key")
    configure_file(
        ${CERTS_TEMPLATE}
        ${CMAKE_CURRENT_LIST_DIR}/certs.c
        @ONLY
    )
    idf_component_register(
        SRCS
            "certs.c"
        INCLUDE_DIRS
            "include"
        EMBED_TXTFILES
            "${SERVER_CERT}"
            "${CLIENT_CERT}"
            "${CLIENT_KEY}"
    )
else()
    set(CERTS_TEMPLATE ${CMAKE_CURRENT_LIST_DIR}/certs.c.external)
    configure_file(
        ${CERTS_TEMPLATE}
        ${CMAKE_CURRENT_LIST_DIR}/certs.c
        @ONLY
    )
    idf_component_register(
        SRCS
            "certs.c"
        INCLUDE_DIRS
            "include"
    )
endif()