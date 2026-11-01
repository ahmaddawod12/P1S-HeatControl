idf_component_register(
    SRCS "firmware_policy.c" "firmware_identity.c" "firmware_manifest.c"
        "firmware_transfer.c" "firmware_transport.c" "firmware_job.c" "firmware_update.c"
        "firmware_trial.c" "firmware_trial_policy.c"
    INCLUDE_DIRS "include" "."
    REQUIRES app_types
    PRIV_REQUIRES esp_app_format app_update esp_http_client mbedtls
        esp_netif esp_timer runtime device_config wifi_manager settings lwip espressif__cjson
)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u custom_app_desc")
idf_build_get_property(p1s_firmware_version PROJECT_VER)
target_compile_definitions(${COMPONENT_LIB} PRIVATE P1S_FIRMWARE_VERSION="${p1s_firmware_version}")
if(P1S_BOOTSTRAP_CREDENTIALS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE P1S_PERSONAL_BOOTSTRAP=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE P1S_PERSONAL_BOOTSTRAP=0)
endif()
