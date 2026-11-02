idf_component_register(SRCS "printer_cycle.c" "printer_control.c" "printer_control_native.c"
    INCLUDE_DIRS "include" "." REQUIRES app_types runtime printer_link
    PRIV_REQUIRES device_config esp_timer esp_hw_support)
