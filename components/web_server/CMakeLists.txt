idf_component_register(
    SRCS
        "http_csrf.c"
        "http_request.c"
        "setup_portal.c"
        "wifi_api_policy.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_http_server
        esp_hw_support
        esp_netif
        esp_timer
        lwip
        espressif__cjson
        web
        wifi_manager
)
