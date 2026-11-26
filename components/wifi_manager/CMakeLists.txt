idf_component_register(
    SRCS
        "wifi_manager.c"
        "wifi_engine.c"
        "wifi_credentials.c"
        "wifi_ap_shutdown.c"
    INCLUDE_DIRS "include"
    REQUIRES device_config
    PRIV_REQUIRES
        bootloader_support
        esp_hw_support
        runtime
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        nvs_flash
)
