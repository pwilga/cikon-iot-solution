# LIGHT_CMND_LIST and HA_ENTITY_LIST - one entry per configured light - come from cikon_light's
# generated header, built from the device's lights.toml. They used to be assembled here by
# splitting the configuration string a second time, which only lined up with the driver's own
# reading of it as long as both splits were kept identical by hand.

idf_component_register(
    SRCS
        "light_adapter.c"
    INCLUDE_DIRS
        "include"
)
