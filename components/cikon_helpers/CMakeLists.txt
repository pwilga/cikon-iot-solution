idf_component_register(
    SRCS
        "json_parser.c"
        "task_helpers.c"
        "net.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        bootloader_support  # For enum_helpers.h (esp_ota_img_states_t)
)
