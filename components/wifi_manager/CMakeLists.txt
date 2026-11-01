idf_component_register(
    SRCS
        "wifi_manager.c"
        "wifi_engine.c"
        "wifi_credentials.c"
        "wifi_ap_shutdown.c"
    INCLUDE_DIRS "include"
    REQUIRES device_config
    PRIV_REQUIRES
        runtime
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        mdns
        nvs_flash
)
