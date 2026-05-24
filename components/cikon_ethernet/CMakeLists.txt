# CRITICAL: ONLY internal cikon_* components in REQUIRES
# External deps (espressif/*) go in idf_component.yml ONLY

set(COMPONENT_SRCS
    "ethernet.c"
)

# Conditional backend sources based on Kconfig
if(CONFIG_CIKON_ETHERNET_W5500)
    list(APPEND COMPONENT_SRCS "ethernet_backend_w5500.c")
endif()

if(CONFIG_CIKON_ETHERNET_OPENETH)
    list(APPEND COMPONENT_SRCS "ethernet_backend_openeth.c")
endif()

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_eth
        esp_netif
        driver  # For SPI (W5500)
)
