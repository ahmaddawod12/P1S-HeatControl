idf_component_register(
    SRCS "firebase_client.c" "firebase_commands.c" "firebase_protocol.c" "firebase_https.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_types clock_service device_config esp_http_client esp_hw_support esp_timer esp-tls
        espressif__cjson firmware_update heap lwip mbedtls printer_control runtime tcp_transport web_server wifi_manager
)
