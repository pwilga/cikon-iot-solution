# The per-light command and entity lists come from the driver's generated header, so they line up
# with its own table by construction rather than by being kept in step by hand.

idf_component_register(
    SRCS
        "light_adapter.c"
    INCLUDE_DIRS
        "include"
)
