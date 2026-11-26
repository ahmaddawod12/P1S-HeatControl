idf_component_register(
    SRCS "app_startup.c"
    INCLUDE_DIRS "include"
    REQUIRES app_types
    PRIV_REQUIRES
        board
        runtime
        settings
        wifi_manager
        nvs_flash
        firmware_update
        firebase_client
        printer_link
        printer_control
)
