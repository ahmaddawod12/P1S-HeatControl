idf_component_register(SRCS "clock_service.c" "clock_native.c" INCLUDE_DIRS "include" "."
    REQUIRES esp_common PRIV_REQUIRES esp_netif esp_timer)
