idf_component_register(
    SRCS "temperature_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES app_types esp_driver_gpio espressif__ds18b20 espressif__onewire_bus
)
