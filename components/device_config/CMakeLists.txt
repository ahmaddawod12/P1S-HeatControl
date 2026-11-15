idf_component_register(SRCS "device_config.c" "device_name.c" INCLUDE_DIRS "include"
    REQUIRES app_types PRIV_REQUIRES nvs_flash)
