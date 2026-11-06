idf_component_register(
    SRCS
        "app_runtime.c"
        "runtime_output_policy.c"
        "runtime_auto_lease.c"
        "service_operation.c"
    INCLUDE_DIRS "include"
    REQUIRES
        app_types
        temperature_sensor
    PRIV_REQUIRES
        board
        controller
        diagnostics
        temperature
        settings
        esp_timer
)
