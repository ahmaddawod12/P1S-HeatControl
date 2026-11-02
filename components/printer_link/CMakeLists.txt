idf_component_register(SRCS "printer_report.c" "printer_status.c" "printer_link.c" "printer_transport.c"
    "printer_deadline_transport.c" "printer_socket.c"
    INCLUDE_DIRS "include" "." REQUIRES app_types
    PRIV_REQUIRES espressif__mqtt esp_netif esp_wifi esp_event esp_timer clock_service tcp_transport lwip
    EMBED_TXTFILES "bambu_ca.pem")
