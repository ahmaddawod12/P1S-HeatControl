idf_component_register(
    SRCS "app_startup.c"
    INCLUDE_DIRS "include"
    REQUIRES app_types
    PRIV_REQUIRES
        board
        runtime
        settings
        web_server
        wifi_manager
        nvs_flash
        firmware_update
        printer_link
        printer_control
)
